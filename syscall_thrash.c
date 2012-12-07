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
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned int num_cores;
static unsigned int num_data_dumpers;
static unsigned int watcher_multiplier;
static unsigned int num_watcher_threads;
static unsigned int num_closer_threads;
static unsigned int num_zero_closers;
static unsigned int num_file_creaters;
static unsigned int num_inotify_instances;

static char *working_dir = "/tmp/inotify_syscall_thrash";
static char *mnt_src;
static char *fstype = "tmpfs";

static pthread_attr_t attr;
static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_var = PTHREAD_COND_INITIALIZER;
static int wait;

struct watcher_struct {
	int inotify_fd;
	int file_num;
};

struct operator_struct {
	int inotify_fd;
};

struct thread_data {
	int inotify_fd;
	pthread_t *watchers;
};

static int stopped = 0;

static int high_wd = 0;
static int low_wd = INT_MAX;

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
static void *create_files(__attribute__ ((unused)) void *ptr)
{
	char filename[50];
	unsigned int i;

	fprintf(stderr, "Starting creator thread\n");

	while (!stopped) {
		for (i = 0; i < num_watcher_threads; i++) {
			int fd;

			snprintf(filename, 50, "%s/%d", working_dir, i);
			unlink(filename);
			fd = open(filename, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR);
			if (fd >= 0)
				close(fd);
		}
		sleep(10);
	}

	/* cleanup all files on exit */
	for (i = 0; i < num_watcher_threads; i++) {
		snprintf(filename, 50, "%s/%d", working_dir, i);
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
static void *__add_watches(void *ptr)
{
	struct watcher_struct *watcher_arg = ptr;
	int file_num = watcher_arg->file_num;
	int notify_fd = watcher_arg->inotify_fd;
	int ret;
	char filename[50];

	fprintf(stderr, "Creating an watch adder thread, notify_fd=%d filenum=%d\n",
		notify_fd, file_num);

	snprintf(filename, 50, "%s/%d", working_dir, file_num);

	pthread_mutex_lock(&wait_mutex);
	wait = 1;
	pthread_cond_signal(&wait_var);
	pthread_mutex_unlock(&wait_mutex);

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

static int add_watches(struct thread_data *td)
{
	struct watcher_struct ws;
	unsigned int i, j;
	int rc;
	pthread_t *watchers;

	ws.inotify_fd = td->inotify_fd;

	/* allocate the pthread_t's for all of the threads */
	watchers = calloc(num_watcher_threads * watcher_multiplier, sizeof(*watchers));
	if (!watchers)
		handle_error("allocating watchers");
	td->watchers = watchers;

	for (i = 0; i < num_watcher_threads; i++) {
		ws.file_num = i;
		for (j = 0; j < watcher_multiplier; j++) {
			rc = pthread_create(&watchers[i * num_watcher_threads + j], &attr, __add_watches, &ws);
			if (rc)
				handle_error("creating water threads");
			pthread_mutex_lock(&wait_mutex);
			if (wait == 0)
				pthread_cond_wait(&wait_var, &wait_mutex);
			wait = 0;
			pthread_mutex_unlock(&wait_mutex);
		}
	}

	return 0;
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
		rc = mount(working_dir, working_dir, "tmpfs", MS_MGC_VAL, "rootcontext=\"unconfined_u:object_r:tmp_t:s0\"");
		usleep(100000);
		if (!rc)
			umount2(working_dir, MNT_DETACH);
		usleep(100000);
	}
	return NULL;
}

static int join_threads(struct thread_data *td)
{
	unsigned int i, j;
	void *ret;
	pthread_t *watchers = td->watchers;

	for (i = 0; i < num_watcher_threads; i++)
		for (j = 0; j < watcher_multiplier; j++)
			pthread_join(watchers[i * num_watcher_threads + j], &ret);

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

	num_cores = 2;
	num_data_dumpers = 4;
	watcher_multiplier = 4;
	num_zero_closers = 1;
	num_file_creaters = 2;
	num_inotify_instances = 2;
	mnt_src = working_dir;
	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
		    {"cores",	required_argument,	0, 'c'},
		    {"data",	required_argument,	0, 'd'},
		    {"multiplier", required_argument,	0, 'm'},
		    {"zero",	required_argument,	0, 'z'},
		    {"creaters", required_argument,	0, 'r'},
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
			str_to_uint(&num_cores, optarg);
			break;
		case 'm':
			str_to_uint(&num_cores, optarg);
			break;
		case 'z':
			str_to_uint(&num_cores, optarg);
			break;
		case 'r':
			str_to_uint(&num_cores, optarg);
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

	if (num_watcher_threads == 0)
		num_watcher_threads = num_cores;
	if (num_closer_threads == 0)
		num_closer_threads = num_watcher_threads * 2;

	return 0;
}

int main(int argc, char *argv[])
{
	int *inotify_fd;
	struct operator_struct *operator_arg;
	struct thread_data *td;
	int iret;
	void *ret;
	unsigned int i, j;
	struct sigaction setmask;
	pthread_t *closers;
	pthread_t *zero_closers;
	pthread_t *data_dumpers;
	pthread_t *file_creaters;
	pthread_t low_wd_reseter;
	pthread_t mounter;

	num_cores = 2;
	num_data_dumpers = 4;
	watcher_multiplier = 4;
	num_watcher_threads = num_cores;
	num_closer_threads = num_watcher_threads * 2;
	num_zero_closers = 1;
	num_file_creaters = 2;
	num_inotify_instances = 2;

	iret = process_args(argc, argv);
	if (iret)
		handle_error("processing arguments");

	/* close cleanly on cntl+c */
	sigemptyset( &setmask.sa_mask );
	setmask.sa_handler = sigfunc;
	setmask.sa_flags   = 0;
	sigaction( SIGINT,  &setmask, (struct sigaction *) NULL );

	/* make sure the directory exists */
	mkdir(working_dir, S_IRWXU);

	/* set up a pthread attr with a tiny stack */
	iret = pthread_attr_init(&attr);
	if (iret)
		handle_error("pthread_attr_init");
	iret = pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN*2);
	if (iret)
		handle_error("pthread_attr_setstacksize");

	iret = pthread_mutex_init(&wait_mutex, NULL);
	if (iret)
		handle_error("initializing mutex");

	td = calloc(num_inotify_instances, sizeof(*td));
	if (!td)
		handle_error("allocating inotify td array");

	inotify_fd = calloc(num_inotify_instances, sizeof(*inotify_fd)); //REMOVE ME
	if (!td)
		handle_error("allocating inotify fd array");

	/* create an inotify instance and make it O_NONBLOCK */
	for (i = 0; i < num_inotify_instances; i++) {
		int fd = inotify_init1(O_NONBLOCK);
		if (fd < 0)
			handle_error("opening inotify_fd");
		inotify_fd[i] = td[i].inotify_fd = fd;

		iret = add_watches(&td[i]);
		if (iret)
			handle_error("creating watches");
	}

	operator_arg = calloc(num_inotify_instances, sizeof(struct operator_struct));
	if (!operator_arg)
		handle_error("allocating operator_arg");

	closers = calloc(num_inotify_instances * num_closer_threads, sizeof(pthread_t));
	if (!closers)
		handle_error("allocating closers");

	zero_closers = calloc(num_inotify_instances * num_zero_closers, sizeof(pthread_t));
	if (!zero_closers)
		handle_error("allocating zero_closers");

	data_dumpers = calloc(num_inotify_instances * num_data_dumpers, sizeof(pthread_t));
	if (!data_dumpers)
		handle_error("allocating data_dumpers");

	file_creaters = calloc(num_file_creaters, sizeof(*file_creaters));
	if (!file_creaters)
		handle_error("allocating file_creaters");

	/* create a thread that does nothing but reset the low_wd */
	iret = pthread_create(&low_wd_reseter, &attr, reset_low_wd, NULL);
	if (iret)
		handle_error("low_wd_reseter");

	iret = pthread_create(&mounter, &attr, mount_tmpdir, NULL);
	if (iret)
		handle_error("low_wd_reseter");

	for (i = 0; i < num_inotify_instances; i++)
		operator_arg[i].inotify_fd = inotify_fd[i];

	/* create threads which unlink and then recreate all of the files in question */
	for (i = 0; i < num_file_creaters; i++) {
		iret = pthread_create( &file_creaters[i], &attr, create_files, NULL);
		if (iret)
			handle_error("creating the file creators");
	}

	/* create threads which walk from low_wd to high_wd closing all of the wd's in between */
	for (i = 0; i < num_inotify_instances; i++)
		for (j = 0; j < num_closer_threads; j++) {
			iret = pthread_create ( &closers[i * num_closer_threads + j], &attr, close_watches, &operator_arg[i]);
			if (iret)
				handle_error("creating the close threads");
		}

	/* create threads which just walk from low_wd to low_wd +3 closing wd's for extra races */
	for (i = 0; i < num_inotify_instances; i++)
		for (j = 0; j < num_zero_closers; j++) {
			iret = pthread_create ( &zero_closers[i * num_zero_closers + j], &attr, zero_close_watches, &operator_arg[i]);
			if (iret)
				handle_error("creating the low closer threads");
		}

	/* create threads which just pull data off of the inotify fd.
	 * use default ATTR for larger stack */
	for (i = 0; i < num_inotify_instances; i++)
		for (j = 0; j < num_data_dumpers; j++) {
			iret = pthread_create( &data_dumpers[i * num_data_dumpers + j], NULL, dump_data, &operator_arg[i]);
			if (iret)
				handle_error("creating threads to dump inotify data");
		}


	/* Wait till threads are complete before main continues. */
	pthread_join(low_wd_reseter, &ret);

	for (i = 0; i < num_inotify_instances; i++)
		join_threads(&td[i]);

	for (i = 0; i < num_file_creaters; i++)
		pthread_join(file_creaters[i], &ret);

	for (i = 0; i < num_inotify_instances; i++)
		for (j = 0; j < num_closer_threads; j++)
			pthread_join(closers[i * num_closer_threads + j], &ret);

	for (i = 0; i < num_inotify_instances; i++)
		for (j = 0; j < num_zero_closers; j++)
			pthread_join(zero_closers[i * num_zero_closers + j], &ret);

	for (i = 0; i < num_inotify_instances; i++)
		for (j = 0; j < num_data_dumpers; j++)
			pthread_join(data_dumpers[i * num_data_dumpers + j], &ret);

	/* clean up the tmp dir which should be empty */
	rmdir(working_dir);

	exit(EXIT_SUCCESS);
}
