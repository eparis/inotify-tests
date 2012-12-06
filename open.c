#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	int i;
	int *fds;

	if (argc < 2) {
		fprintf(stderr, "useage: %s [filenames]\n", argv[0]);
		return 1;
	}

	fds = malloc(sizeof(int) * argc-1);
	if (!fds) {
		fprintf(stderr, "ENOMEM for fd array\n");
		return 1;
	}

	for (i = 0; i < argc-1; i++) {
		fds[i] = open(argv[i+1], O_RDONLY);
		if (fds[i] < 0) {
			perror("Opening one of the files");
			return 1;
		}
	}

	while (1) {
		sleep(10);
	}
	return 0;
}
