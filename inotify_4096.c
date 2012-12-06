#include <sys/inotify.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

int main(void)
{
	int fd = inotify_init();
	int i;

	if (fd < 0)
		abort();

	for (i = 0; i < 5000; i++) {
		int one = inotify_add_watch(fd, ".", IN_MODIFY);
		int two = inotify_add_watch(fd, "..", IN_MODIFY);
		int s;

		if (one < 0) {
			printf("failed to add_watch one=%d\n", one);
			abort();
		}

		if (two < 0) {
			printf("failed to add_watch two=%d\n", two);
			abort();
		}

		printf("one=%d two=%d\n", one, two);

		s = inotify_rm_watch(fd, one);
		if (s != 0) {
			printf("failed to rm_watch wd %d: %s\n",
			       one, strerror(errno));
			abort();
		}

		s = inotify_rm_watch(fd, two);
		if (s != 0) {
			printf("failed to rm_watch wd %d: %s\n",
			       two, strerror(errno));
			abort();
		}
	}
	return 0;
}
