CFLAGS += -Wall -W -g

all: syscall_thrash inotify_4096 inotify-oneshot inotify-unlink

syscall_thrash: syscall_thrash.c Makefile
	gcc -o syscall_thrash $(CFLAGS) -lpthread syscall_thrash.c

inotify_4096: inotify_4096.c Makefile
	gcc -o inotify_4096 $(CFLAGS) inotify_4096.c

inotify-oneshot: Makefile inotify-oneshot.c
	gcc -o inotify-oneshot $(CFLAGS) inotify-oneshot.c

inotify-unlink: Makefile inotify-unlink.c
	gcc -o inotify-unlink $(CFLAGS) inotify-unlink.c

clean:
	rm -f syscall_thrash inotify_4096 inotify-oneshot inotify-unlink
