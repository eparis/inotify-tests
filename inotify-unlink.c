#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char *argv[] __attribute__ ((unused)))
{
	int i, ifd, ret, tfd, wd;
	ssize_t len;
	FILE *ifile;
	struct inotify_event ie;
	char buf[sizeof(struct inotify_event)];
	uint32_t mask = IN_ALL_EVENTS;

	assert(sizeof(struct inotify_event) > sizeof("hello"));

	/* set up inotify */
	ifd = inotify_init();
	if (ifd < 0) {
		perror("inotify_init");
		return 1;
	}

	ifile = fdopen(ifd, "r");
	if (!ifile) {
		perror("fdopen");
		return 1;
	}

	if (argc > 1)
		mask |= 0x04000000;

	ret = mkdir("/tmp/inotify", S_IRWXU);

	wd = inotify_add_watch(ifd, "/tmp/inotify", mask);
	if (wd < 0) {
		perror("inotify_add_watch");
		return 1;
	}

	/* create, unlink, modify, access a file */
	tfd = open("/tmp/inotify/unlinkme", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (tfd < 0) {
		perror("open");
		return 1;
	}

	ret = unlink("/tmp/inotify/unlinkme");
	if (ret) {
		perror("unlink");
		return 1;
	}

	/* generate a lot of events */
	for (i = 0; i < 5; i++) {
		lseek(tfd, 0, SEEK_SET);

		len = write(tfd, "hello", 6);
		if (len < 0) {
			perror("write");
			return 1;
		}
	
		lseek(tfd, 0, SEEK_SET);

		len = read(tfd, buf, sizeof(buf));
		if (len < 0) {
			perror("read");
			return 1;
		}
	}

	/*check what inotify events we got */
	while(fread(&ie, sizeof(ie), 1, ifile)) {
		printf("wd=%d mask=%x", ie.wd, ie.mask);
		if (ie.len) {
			assert(ie.len <= sizeof(buf));
			if (!fread(buf, ie.len, 1, ifile)) {
				perror("fread-ing name");
				return 1;
			}
			printf(" name=%s", buf);
		}
		printf("\n");
	}

	rmdir("/tmp/inotify");

	return 0;
}
