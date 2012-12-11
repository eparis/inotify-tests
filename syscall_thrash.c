#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* huerristic on how hard to load a box */
static unsigned int num_cores;
/* number of threads which just read from inotify_fd and ignore the results */
static unsigned int num_data_dumpers;
/* each thread will add a watch to a given file as fast as it can */
static unsigned int num_adder_threads;
/* each thread will remove all watches between low and high as fast as they can */
static unsigned int num_remover_threads;
/* a multiplier for adders and removers so you have X adders per file */
static unsigned int watcher_multiplier;
/* this removes watches from low_wd to low_wd + 3 just for extra removal races */
static unsigned int num_low_remover_threads;
/* create and destroy the files watches are being added to and removed from */
static unsigned int num_file_creaters;
/* how many inotify_fd's we have total (so another multiplier for adders and removers) */
static unsigned int num_inotify_instances;

static char *working_dir = "/tmp/inotify_syscall_thrash";
/* if mounting a real filesystem, where is the source?  (doesn't matter for tmpfs) */
static char *mnt_src;
/* what is the fstype to mount and unmonut? */
static char *fstype = "tmpfs";

static pthread_attr_t attr;

static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_var = PTHREAD_COND_INITIALIZER;
static int wait;
#define WAIT_CHILD do {\
		pthread_mutex_lock(&wait_mutex); \
		if (wait == 0) \
			pthread_cond_wait(&wait_var, &wait_mutex); \
		wait = 0; \
		pthread_mutex_unlock(&wait_mutex); \
	} while (0);
#define WAKE_PARENT do {\
		pthread_mutex_lock(&wait_mutex); \
		wait = 1; \
		pthread_cond_signal(&wait_var); \
		pthread_mutex_unlock(&wait_mutex); \
	} while (0);

struct adder_struct {
	int inotify_fd;
	int file_num;
};

struct operator_struct {
	int inotify_fd;
};

struct thread_data {
	int inotify_fd;
	pthread_t *adders;
	pthread_t *removers;
	pthread_t *lownum_removers;
	pthread_t *data_dumpers;
};

pthread_t *file_creaters;
pthread_t low_wd_reseter;
pthread_t mounter;

static int stopped = 0;

static int high_wd = 0;
static int low_wd = INT_MAX;

static int handle_error(const char *arg)
{
	perror(arg);
	exit(EXIT_FAILURE);
}

static void sigfunc(int sig_num)
{
	if (sig_num == SIGINT)
		stopped = 1;
	else
		printf("Got an unknown signal!\n");
}

/* constantly create and delete all of the files that are bieng watched */
static void *__create_files(__attribute__ ((unused)) void *ptr)
{
	char filename[50];
	unsigned int i;

	fprintf(stdout, "Starting creater thread\n");

	WAKE_PARENT;

	while (!stopped) {
		for (i = 0; i < num_adder_threads; i++) {
			int fd;

			snprintf(filename, 50, "%s/%d", working_dir, i);
			unlink(filename);
			fd = open(filename, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR);
			if (fd >= 0)
				close(fd);
		}
		sleep(2);
	}

	/* cleanup all files on exit */
	for (i = 0; i < num_adder_threads; i++) {
		snprintf(filename, 50, "%s/%d", working_dir, i);
		unlink(filename);
	}

	return NULL;
}

static int start_file_creater_threads(void)
{
	int rc;
	unsigned int i;

	file_creaters = calloc(num_file_creaters, sizeof(*file_creaters));
	if (!file_creaters)
		handle_error("allocating file creater pthreads");

	/* create threads which unlink and then recreate all of the files in question */
	for (i = 0; i < num_file_creaters; i++) {
		rc = pthread_create(&file_creaters[i], &attr, __create_files, NULL);
		if (rc)
			handle_error("creating the file creater threads");
		WAIT_CHILD;
	}
	return 0;
}

/* Reset the low_wd so removers can be smart */
static void *__reset_low_wd(__attribute__ ((unused)) void *ptr)
{
	fprintf(stdout, "Starting low_wd reset thread\n");

	WAKE_PARENT;

	while (!stopped) {
		low_wd = INT_MAX;
		sleep(1);
	}

	return NULL;
}

static int start_reset_low_wd_thread(void)
{
	int rc;

	/* create a thread that does nothing but reset the low_wd */
	rc = pthread_create(&low_wd_reseter, &attr, __reset_low_wd, NULL);
	if (rc)
		handle_error("low_wd_reseter");

	WAIT_CHILD;

	return 0;
}

/* Pull events off the buffer and ignore them */
static void *__dump_data(void *ptr)
{
	char buf[8096];
	struct operator_struct *operator_arg = ptr;
	int inotify_fd = operator_arg->inotify_fd;
	int ret;

	fprintf(stdout, "Starting inotify data dumper thread\n");

	WAKE_PARENT;

	while (!stopped) {
		ret = read(inotify_fd, buf, 8096);
		if (ret <= 0)
			pthread_yield();
	}

	return NULL;
}

/* create threads which just pull data off of the inotify fd. */
static int start_data_dumping_threads(struct thread_data *td)
{
	struct operator_struct os;
	unsigned int i;
	int rc;
	pthread_t *data_dumpers;

	os.inotify_fd = td->inotify_fd;

	/* allocate the pthread_t's for all of the threads */
	data_dumpers = calloc(num_data_dumpers, sizeof(*data_dumpers));
	if (!data_dumpers)
		handle_error("allocating data_dumpers");
	td->data_dumpers = data_dumpers;

	/* use default ATTR for larger stack */
	for (i = 0; i < num_data_dumpers; i++) {
		rc = pthread_create(&data_dumpers[i], NULL, __dump_data, &os);
		if (rc)
			handle_error("creating threads to dump inotify data");
		WAIT_CHILD;
	}
	return 0;
}

/* add a watch to a specific file as fast as we can */
static void *__add_watches(void *ptr)
{
	struct adder_struct *adder_arg = ptr;
	int file_num = adder_arg->file_num;
	int notify_fd = adder_arg->inotify_fd;
	int ret;
	char filename[50];

	fprintf(stdout, "Creating a watch creater thread, notify_fd=%d filenum=%d\n",
		notify_fd, file_num);

	snprintf(filename, 50, "%s/%d", working_dir, file_num);

	WAKE_PARENT;

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

static int start_watch_creation_threads(struct thread_data *td)
{
	struct adder_struct ws;
	unsigned int i, j;
	int rc;
	pthread_t *adders;

	ws.inotify_fd = td->inotify_fd;

	/* allocate the pthread_t's for all of the threads */
	adders = calloc(num_adder_threads * watcher_multiplier, sizeof(*adders));
	if (!adders)
		handle_error("allocating adders");
	td->adders = adders;

	for (i = 0; i < num_adder_threads; i++) {
		ws.file_num = i;
		for (j = 0; j < watcher_multiplier; j++) {
			rc = pthread_create(&adders[i * watcher_multiplier + j], &attr, __add_watches, &ws);
			if (rc)
				handle_error("creating water threads");
			WAIT_CHILD;
		}
	}

	return 0;
}

/* run from low_wd to high_wd removing all watches in between */
static void *__remove_watches(void *ptr)
{
	struct operator_struct *operator_arg = ptr;
	int inotify_fd = operator_arg->inotify_fd;
	int i;

	fprintf(stdout, "Starting a thread to remove watches\n");

	WAKE_PARENT;

	while (!stopped) {
		for (i = low_wd; i < high_wd; i++)
			inotify_rm_watch(inotify_fd, i);
		pthread_yield();
	}
	return NULL;
}

static int start_watch_removal_threads(struct thread_data *td)
{
	struct operator_struct os;
	int rc;
	unsigned int i, j;
	pthread_t *removers;

	os.inotify_fd = td->inotify_fd;

	/* allocate the pthread_t's for all of the threads */
	removers = calloc(num_remover_threads * watcher_multiplier, sizeof(*removers));
	if (!removers)
		handle_error("allocating removal pthreads");

	td->removers = removers;

	/* create threads which walk from low_wd to high_wd closing all of the wd's in between */
	for (i = 0; i < num_remover_threads; i++) {
		for (j = 0; j < watcher_multiplier; j++) {
			rc = pthread_create(&removers[i * watcher_multiplier + j], &attr, __remove_watches, &os);
			if (rc)
				handle_error("creating the removal threads");
			WAIT_CHILD;
		}
	}
	return 0;
}

/* run from low_wd to low_wd+3 closing all watch in between just for extra races */
static void *__remove_lownum_watches(void *ptr)
{
	struct operator_struct *operator_arg = ptr;
	int inotify_fd = operator_arg->inotify_fd;
	int i;

	fprintf(stdout, "Starting thread to remove low watches\n");

	WAKE_PARENT;

	while (!stopped) {
		for (i = low_wd; i <= low_wd+3; i++)
			inotify_rm_watch(inotify_fd, i);
		pthread_yield();
	}
	return NULL;
}

static int start_lownum_watch_removal_threads(struct thread_data *td)
{
	struct operator_struct od;
	int rc;
	unsigned int i;
	pthread_t *lownum_removers;

	od.inotify_fd = td->inotify_fd;

	lownum_removers = calloc(num_low_remover_threads, sizeof(*lownum_removers));
	if (!lownum_removers)
		handle_error("allocating lownum removal pthreads");

	td->lownum_removers = lownum_removers;

	/* create threads which walk from low_wd to high_wd closing all of the wd's in between */
	for (i = 0; i < num_low_remover_threads; i++) {
		rc = pthread_create (&lownum_removers[i], &attr, __remove_lownum_watches, &od);
		if (rc)
			handle_error("creating the lownum removal threads");
		WAIT_CHILD;
	}
	return 0;
}

static void *__mount_fs(__attribute__ ((unused)) void *ptr)
{
	int rc;
 
	fprintf(stdout, "Starting mount and unmount fs on top of working dir\n");

	WAKE_PARENT;

	while (!stopped) {
		rc = mount(mnt_src, working_dir, fstype, MS_MGC_VAL, "rootcontext=\"unconfined_u:object_r:tmp_t:s0\"");
		usleep(100000);
		if (!rc)
			umount2(working_dir, MNT_DETACH);
		else
			fprintf(stderr, "Failed to mount %s: %s\n", mnt_src, strerror(errno));
		usleep(100000);
	}
	return NULL;
}

static int start_mount_fs_thread(void)
{
	int rc;

	rc = pthread_create(&mounter, &attr, __mount_fs, NULL);
	if (rc)
		handle_error("creating the thread to mount and unmount an fs");
	WAIT_CHILD;

	return 0;
}

static int join_threads(struct thread_data *td)
{
	unsigned int i, j;
	void *ret;
	pthread_t *to_join;

	to_join = td->adders;
	for (i = 0; i < num_adder_threads; i++)
		for (j = 0; j < watcher_multiplier; j++)
			pthread_join(to_join[i * watcher_multiplier + j], &ret);

	to_join = td->removers;
	for (i = 0; i < num_remover_threads; i++)
		for (j = 0; j < watcher_multiplier; j++)
			pthread_join(to_join[i * watcher_multiplier + j], &ret);

	to_join = td->lownum_removers;
	for (i = 0; i < num_low_remover_threads; i++)
		pthread_join(to_join[i], &ret);

	to_join = td->data_dumpers;
	for (i = 0; i < num_data_dumpers; i++)
		pthread_join(to_join[i], &ret);

	return 0;
}

static int str_to_uint(unsigned int *out, char *in)
{
	long val;
	char *endptr;

	errno = 0;    /* To distinguish success/failure after call */
	val = strtol(in, &endptr, 10);

	/* Check for various possible errors */
	if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN)) || (errno != 0 && val == 0)) {
		perror("strtol");
		return -1;
	}

	if (endptr == in) {
		fprintf(stderr, "No digits were found\n");
		return -1;
	}

	if (*endptr != '\0') { /* random shit after the number? */
		printf("Further characters after number: %s\n", endptr);
		return -1;
	}

	*out = val;

	return 0;
}

static int process_args(int argc, char *argv[])
{
	int c;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
		    {"cores",	required_argument,	0, 'c'},
		    {"data",	required_argument,	0, 'd'},
		    {"multiplier", required_argument,	0, 'm'},
		    {"low",	required_argument,	0, 'l'},
		    {"adders", required_argument,	0, 'a'},
		    {"instances", required_argument,	0, 'i'},
		    {"dir", required_argument,		0, 't'},
		    {"source_mnt", required_argument,	0, 's'},
		    {"fstype", required_argument,	0, 'f'},
		    {0,		0,			0,  0 }
		};

		c = getopt_long(argc, argv, "c:d:m:z:r:i:t:s:f:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			str_to_uint(&num_cores, optarg);
			break;
		case 'd':
			str_to_uint(&num_data_dumpers, optarg);
			break;
		case 'm':
			str_to_uint(&watcher_multiplier, optarg);
			break;
		case 'l':
			str_to_uint(&num_low_remover_threads, optarg);
			break;
		case 'a':
			str_to_uint(&num_adder_threads, optarg);
			break;
		case 'i':
			str_to_uint(&num_cores, optarg);
			break;
		case 't':
			working_dir = optarg;
			break;
		case 's':
			mnt_src = optarg;
			break;
		case 'f':
			fstype = optarg;
			break;
		default:
			printf("?? unknown option 0%o ??\n", c);
			return -1;
		}
	}

	if (optind < argc) {
		printf("non-option ARGV-elements: ");
		while (optind < argc)
			printf("%s ", argv[optind++]);
		printf("\n");
	}

	if (num_cores == 0)
		num_cores = sysconf(_SC_NPROCESSORS_ONLN);
	if (num_cores < 1)
		num_cores = 1;
	num_cores++;

	if (num_data_dumpers == 0)
		num_data_dumpers = 1;

	if (watcher_multiplier == 0)
		watcher_multiplier = 2;

	if (num_low_remover_threads == 0)
		num_low_remover_threads = 1;

	if (num_file_creaters == 0)
		num_file_creaters = num_cores/2;

	if (num_inotify_instances == 0)
		num_inotify_instances = num_cores/2;

	if (mnt_src == NULL)
		mnt_src = working_dir;

	if (num_adder_threads == 0)
		num_adder_threads = 3;

	if (num_remover_threads == 0)
		num_remover_threads = num_adder_threads;

	return 0;
}

int main(int argc, char *argv[])
{
	struct thread_data *td;
	int rc;
	void *ret;
	unsigned int i;
	struct sigaction setmask;

	rc = process_args(argc, argv);
	if (rc)
		handle_error("processing arguments");

	/* close cleanly on cntl+c */
	sigemptyset( &setmask.sa_mask );
	setmask.sa_handler = sigfunc;
	setmask.sa_flags   = 0;
	sigaction( SIGINT,  &setmask, (struct sigaction *) NULL );

	/* make sure the directory exists */
	mkdir(working_dir, S_IRWXU);

	/* set up a pthread attr with a tiny stack */
	rc = pthread_attr_init(&attr);
	if (rc)
		handle_error("pthread_attr_init");
	rc = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN*2);
	if (rc)
		handle_error("pthread_attr_setstacksize");

	td = calloc(num_inotify_instances, sizeof(*td));
	if (!td)
		handle_error("allocating inotify td array");

	/* create an inotify instance and make it O_NONBLOCK */
	for (i = 0; i < num_inotify_instances; i++) {
		struct thread_data *t;
		int fd;

		fd = inotify_init1(O_NONBLOCK);
		if (fd < 0)
			handle_error("opening inotify_fd");

		t = &td[i];
		t->inotify_fd = fd;

		rc = start_watch_creation_threads(t);
		if (rc)
			handle_error("creating watch adding threads");

		rc = start_watch_removal_threads(t);
		if (rc)
			handle_error("creating watch remover threads");

		rc = start_lownum_watch_removal_threads(t);
		if (rc)
			handle_error("creating lownum watch remover threads");

		rc = start_data_dumping_threads(t);
		if (rc)
			handle_error("creating data dumping threads");
	}

	rc = start_file_creater_threads();
	if (rc)
		handle_error("creating file creation/rm threads");

	rc = start_reset_low_wd_thread();
	if (rc)
		handle_error("starting thread to reset the low_wd");

	rc = start_mount_fs_thread();
	if (rc)
		handle_error("starting mounting thread");

	/* join the per inotify instance threads */
	for (i = 0; i < num_inotify_instances; i++)
		join_threads(&td[i]);

	for (i = 0; i < num_file_creaters; i++)
		pthread_join(file_creaters[i], &ret);

	pthread_join(low_wd_reseter, &ret);
	pthread_join(mounter, &ret);

	/* clean up the tmp dir which should be empty */
	rmdir(working_dir);

	for (i = 0; i < num_inotify_instances; i++) {
		free(td[i].adders);
		free(td[i].removers);
		free(td[i].lownum_removers);
		free(td[i].data_dumpers);
	}
	free(td);
	free(file_creaters);
	exit(EXIT_SUCCESS);
}
