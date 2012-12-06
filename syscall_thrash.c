#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define NUM_CORES		2
#define NUM_DATA_DUMPERS	4
#define WATCHER_MULTIPLIER	4
#define NUM_WATCHER_THREADS 	NUM_CORES
#define NUM_CLOSER_THREADS	NUM_WATCHER_THREADS * 2
#define NUM_ZERO_CLOSERS	1
#define NUM_FILE_CREATERS	2
#define NUM_INOTIFY_INSTANCES	2

#define TMP_DIR_NAME "/tmp/inotify_syscall_thrash"

struct watcher_struct {
	int inotify_fd;
	int file_num;
};

struct operator_struct {
	int inotify_fd;
};

static int stopped = 0;

static int high_wd = 0;
static int low_wd = INT_MAX;

static void *add_watches(void *ptr);
static void *close_watches(void *ptr);
static void *zero_close_watches(void *ptr);
static void *dump_data(void *ptr);
static void *reset_low_wd(void *ptr);
static void *create_files(void *ptr);
static void *mount_tmpdir(void *ptr);
static void sigfunc(int sig_num);

static int handle_error(const char *arg)
{
	perror(arg);
	exit(1);
}

static void sigfunc(int sig_num)
{
	if (sig_num == SIGINT)
		stopped = 1;
	else
		printf("Got an unknown signal!\n");
}

/* constantly create and delete all of the files that are bieng watched */
static void *create_files(__attribute__ ((unused)) void *ptr)
{
	char filename[50];
	int i;

	fprintf(stderr, "Starting creator thread\n");

	while (!stopped) {
		for (i = 0; i < NUM_WATCHER_THREADS; i++) {
			int fd;

			snprintf(filename, 50, "%s/%d", TMP_DIR_NAME, i);
			unlink(filename);
			fd = open(filename, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR);
			if (fd >= 0)
				close(fd);
		}
		sleep(10);
	}

	/* cleanup all files on exit */
	for (i = 0; i < NUM_WATCHER_THREADS; i++) {
		snprintf(filename, 50, "%s/%d", TMP_DIR_NAME, i);
		unlink(filename);
	}

	return NULL;
}

/* Reset the low_wd so closers can be smart */
static void *reset_low_wd(__attribute__ ((unused)) void *ptr)
{
	fprintf(stderr, "Starting low_wd reset thread\n");

	while (!stopped) {
		low_wd = INT_MAX;
		sleep(1);
	}

	return NULL;
}

/* Pull events off the buffer and ignore them */
static void *dump_data(void *ptr)
{
	char buf[8096];
	struct operator_struct *operator_arg = ptr;
	int inotify_fd = operator_arg->inotify_fd;
	int ret;

	fprintf(stderr, "Starting inotify data dumper thread\n");

	while (!stopped) {
		ret = read(inotify_fd, buf, 8096);
		if (ret <= 0)
			pthread_yield();
	}

	return NULL;
}

/* add a watch to a specific file as fast as we can */
static void *add_watches(void *ptr)
{
	struct watcher_struct *watcher_arg = ptr;
	int file_num = watcher_arg->file_num;
	int notify_fd = watcher_arg->inotify_fd;
	int ret;
	char filename[50];

	fprintf(stderr, "Creating an watch adder thread, notify_fd=%d filenum=%d\n",
		notify_fd, file_num);

	snprintf(filename, 50, "%s/%d", TMP_DIR_NAME, file_num);

	while (!stopped) {
		ret = inotify_add_watch(notify_fd, filename, IN_ALL_EVENTS);
		if (ret < 0 && errno != ENOENT)
			perror("inotify_add_watch");
		if (ret > high_wd)
			high_wd = ret;
		if (ret < low_wd)
			low_wd = ret;
		pthread_yield();
	}

	return NULL;
}

/* run from low_wd to high_wd closing all watches in between */
static void *close_watches(void *ptr)
{
	struct operator_struct *operator_arg = ptr;
	int inotify_fd = operator_arg->inotify_fd;
	int i;

	fprintf(stderr, "Starting a thread to close watchers\n");

	while (!stopped) {
		for (i = low_wd; i < high_wd; i++)
			inotify_rm_watch(inotify_fd, i);
		pthread_yield();
	}
	return NULL;
}

/* run from low_wd to low_wd+3 closing all watch in between just for extra races */
static void *zero_close_watches(void *ptr)
{
	struct operator_struct *operator_arg = ptr;
	int inotify_fd = operator_arg->inotify_fd;
	int i;
	while (!stopped) {
		for (i = low_wd; i <= low_wd+3; i++)
			inotify_rm_watch(inotify_fd, i);
		pthread_yield();
	}
	return NULL;
}

static void *mount_tmpdir(__attribute__ ((unused)) void *ptr)
{
	int rc;

	while (!stopped) {
		rc = mount(TMP_DIR_NAME, TMP_DIR_NAME, "tmpfs", MS_MGC_VAL, "rootcontext=\"unconfined_u:object_r:tmp_t:s0\"");
		usleep(100000);
		if (!rc)
			umount2(TMP_DIR_NAME, MNT_DETACH);
		usleep(100000);
	}
	return NULL;
}

int main(void)
{
	int inotify_fd[NUM_INOTIFY_INSTANCES];
	struct watcher_struct *watcher_arg;
	struct operator_struct *operator_arg;
	pthread_t *watchers;
	pthread_t *closers;
	pthread_t *zero_closers;
	pthread_t *data_dumpers;
	pthread_t *file_creaters;
	pthread_t low_wd_reseter;
	pthread_t mounter;
	pthread_attr_t attr;
	int iret;
	void *ret;
	int i, j, k;
	struct sigaction setmask;

	/* close cleanly on cntl+c */
	sigemptyset( &setmask.sa_mask );
	setmask.sa_handler = sigfunc;
	setmask.sa_flags   = 0;
	sigaction( SIGINT,  &setmask, (struct sigaction *) NULL );

	/* create and inotify instance an make it O_NONBLOCK */
	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++) {
		int fd  = inotify_init();
		if (fd < 0)
			handle_error("opening inotify_fd");
		iret = fcntl(fd, F_SETFL, O_NONBLOCK);
		if (iret)
			handle_error("setting NONBLOCK");
		inotify_fd[i] = fd;
	}

	/* make sure the directory exists */
	mkdir(TMP_DIR_NAME, S_IRWXU);

	/* set up a pthread attr with a tiny stack */
	iret = pthread_attr_init(&attr);
	if (iret)
		handle_error("pthread_attr_init");
	iret = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN*2);
	if (iret)
		handle_error("pthread_attr_setstacksize");

	/* watchers need to know what file to pay with, so we need and argument */
	watcher_arg = calloc(NUM_INOTIFY_INSTANCES * NUM_WATCHER_THREADS, sizeof(struct watcher_struct));
	if (!watcher_arg)
		handle_error("allocating watcher_arg");

	operator_arg = calloc(NUM_INOTIFY_INSTANCES, sizeof(struct operator_struct));
	if (!operator_arg)
		handle_error("allocating operator_arg");

	/* allocate the pthread_t's for all of the threads */
	watchers = calloc(NUM_INOTIFY_INSTANCES * NUM_WATCHER_THREADS * WATCHER_MULTIPLIER, sizeof(pthread_t));
	if (!watchers)
		handle_error("allocating watchers");

	closers = calloc(NUM_INOTIFY_INSTANCES * NUM_CLOSER_THREADS, sizeof(pthread_t));
	if (!closers)
		handle_error("allocating closers");

	zero_closers = calloc(NUM_INOTIFY_INSTANCES * NUM_ZERO_CLOSERS, sizeof(pthread_t));
	if (!zero_closers)
		handle_error("allocating zero_closers");

	data_dumpers = calloc(NUM_INOTIFY_INSTANCES * NUM_DATA_DUMPERS, sizeof(pthread_t));
	if (!data_dumpers)
		handle_error("allocating data_dumpers");

	file_creaters = calloc(NUM_FILE_CREATERS, sizeof(*file_creaters));
	if (!file_creaters)
		handle_error("allocating file_creaters");

	/* create a thread that does nothing but reset the low_wd */
	iret = pthread_create(&low_wd_reseter, &attr, reset_low_wd, NULL);
	if (iret)
		handle_error("low_wd_reseter");

	iret = pthread_create(&mounter, &attr, mount_tmpdir, NULL);
	if (iret)
		handle_error("low_wd_reseter");

	/* create WATCHER_MULTIPLIER threads per file which do nothing
	 * but try to add a watch for each INOTIFY_INSTANCE */
	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++) {
		for (j = 0; j < NUM_WATCHER_THREADS; j++) {
			watcher_arg[i * NUM_WATCHER_THREADS + j].file_num = j;
			watcher_arg[i * NUM_WATCHER_THREADS + j].inotify_fd = inotify_fd[i];
			for (k = 0; k < WATCHER_MULTIPLIER; k++) {
				iret = pthread_create(&watchers[i * (NUM_WATCHER_THREADS * WATCHER_MULTIPLIER) +
								(j * WATCHER_MULTIPLIER) + k],
						      &attr, add_watches, &watcher_arg[i * NUM_WATCHER_THREADS + j]);
				if (iret)
					handle_error("creating water threads");
			}
		}
	}

	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		operator_arg[i].inotify_fd = inotify_fd[i];

	/* create threads which unlink and then recreate all of the files in question */
	for (i = 0; i < NUM_FILE_CREATERS; i++) {
		iret = pthread_create( &file_creaters[i], &attr, create_files, NULL);
		if (iret)
			handle_error("creating the file creators");
	}

	/* create threads which walk from low_wd to high_wd closing all of the wd's in between */
	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		for (j = 0; j < NUM_CLOSER_THREADS; j++) {
			iret = pthread_create ( &closers[i * NUM_CLOSER_THREADS + j], &attr, close_watches, &operator_arg[i]);
			if (iret)
				handle_error("creating the close threads");
		}

	/* create threads which just walk from low_wd to low_wd +3 closing wd's for extra races */
	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		for (j = 0; j < NUM_ZERO_CLOSERS; j++) {
			iret = pthread_create ( &zero_closers[i * NUM_ZERO_CLOSERS + j], &attr, zero_close_watches, &operator_arg[i]);
			if (iret)
				handle_error("creating the low closer threads");
		}

	/* create threads which just pull data off of the inotify fd.
	 * use default ATTR for larger stack */
	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		for (j = 0; j < NUM_DATA_DUMPERS; j++) {
			iret = pthread_create( &data_dumpers[i * NUM_DATA_DUMPERS + j], NULL, dump_data, &operator_arg[i]);
			if (iret)
				handle_error("creating threads to dump inotify data");
		}


	/* Wait till threads are complete before main continues. */
	pthread_join(low_wd_reseter, &ret);

	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		for (j = 0; j < NUM_WATCHER_THREADS; j++)
			for (k = 0; k < WATCHER_MULTIPLIER; k++)
				pthread_join(watchers[i * (NUM_WATCHER_THREADS * WATCHER_MULTIPLIER) +
						      (j * WATCHER_MULTIPLIER) + k], &ret);

	for (i = 0; i < NUM_FILE_CREATERS; i++)
		pthread_join(file_creaters[i], &ret);

	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		for (j = 0; j < NUM_CLOSER_THREADS; j++)
			pthread_join(closers[i * NUM_CLOSER_THREADS + j], &ret);

	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		for (j = 0; j < NUM_ZERO_CLOSERS; j++)
			pthread_join(zero_closers[i * NUM_ZERO_CLOSERS + j], &ret);

	for (i = 0; i < NUM_INOTIFY_INSTANCES; i++)
		for (j = 0; j < NUM_DATA_DUMPERS; j++)
			pthread_join(data_dumpers[i * NUM_DATA_DUMPERS + j], &ret);

	/* clean up the tmp dir which should be empty */
	rmdir(TMP_DIR_NAME);

	exit(0);
}
