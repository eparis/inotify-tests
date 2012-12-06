#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/inotify.h>
#include <unistd.h>

int wd1 = -1;
int should_exit = 0;
int inotify_fd = 0;

static void handler(int sig, siginfo_t *si __attribute__ ((unused)), void *data __attribute__ ((unused)))
{
	int ret;
	if (sig == SIGUSR1) {
		ret = inotify_rm_watch(inotify_fd, wd1);
		if (ret < 0)
			perror("inotify_rm_watch");
	} else if (sig == SIGUSR2) {
		ret = close(inotify_fd);
		if (ret < 0)
			perror("closeing the fd");
		should_exit = 1;
	} else if (sig == SIGINT) {
		should_exit = 1;
	}
	printf("got signal=%d\n", sig);
}

static int print_events(void)
{
	char buf[8192];	
	char *p;
	struct inotify_event *event;
	struct pollfd fds;
	uint32_t *cur;
	int ret, i, event_len;

	fds.fd = inotify_fd;
	fds.events = (POLLIN);

	ret = poll(&fds, 1, 50);
	if (ret < 0) {
		perror("poll");
		return 1;
	} else if (ret == 0)
		return 1;

	ret = read(inotify_fd, buf, 8192);
	if (ret <= 0) {
		perror("read");
		exit(1);
	}

	p = &buf[0];
	while (p < &buf[0] + ret) {
		event = (struct inotify_event *)p;
		cur = (uint32_t *)p;

		event_len = sizeof(struct inotify_event) + event->len;
		/* print the RAW inotify_event */
		for (i = 0; i < event_len / sizeof(uint32_t); i += 4)
			printf("\t%08x  %08x  %08x  %08x\n",
				cur[i+0], cur[i+1], cur[i+2] , cur[i+3]);

		printf("wd=%d mask=%x cookie=%d len=%d", event->wd, event->mask, event->cookie, event->len);
		if (event->len)
			printf(" event->name=%s", event->name);
		printf("\n\n");

		p += sizeof(struct inotify_event) + event->len;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int ret;
	struct sigaction act;
	int wd, i;

	if (argc < 2) {
		printf("usage: %s [FILENAME]\n", argv[0]);
		return 1;
	}

	act.sa_sigaction = handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_SIGINFO;
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGUSR1, &act, NULL);
	sigaction(SIGUSR2, &act, NULL);

	inotify_fd = inotify_init();
	if (inotify_fd < 0) {
		perror("inotify_init");
		return 1;
	}
	printf("inotify fd=%d sizeof(struct inotify_event)=%zd\n", inotify_fd, sizeof(struct inotify_event));

	for (i = 1; i < argc; i++) {
		ret = inotify_add_watch(inotify_fd, argv[i], IN_ALL_EVENTS);
		if (ret < 0) {
			perror("inotify_add_watch");
			return 1;
		} else {
			wd = ret;
			printf("wd=%d for %s\n", wd, argv[i]);
			if (i == 1)
				wd1 = wd;
		}
	}

	while(1) {
		if (should_exit)
			break;
		print_events();
	}

	return 0;
}
