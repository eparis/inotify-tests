/*
 *   compile as:
 *     gcc -Wall -o inotify-test inotify-test.c
 *     */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/inotify.h>
#include <sys/time.h>
#include <poll.h>

int makeFile (const char* filename) {
    FILE *file;
    file = fopen ( filename, "w" );
    if ( file == NULL ) {
        fprintf ( stderr, "Failed to open \"%s\" for writing: %s\n",
            filename, strerror ( errno ) );
        return -1;
    }
    fclose ( file );

    return 0;
}

int main (int argc __attribute__((unused)), char* argv[] __attribute__((unused))) {
    const char filename[] = "/tmp/inotify_oneshot_test.test";
    struct inotify_event event;
    int notifyFD, wd, ret, i;

    if ((notifyFD = inotify_init()) < 0) {
        fprintf(stderr, "inotify_init() failed: %s\n", strerror(errno));
        return 1;
    }

    makeFile ( filename );

    /* create a one shot event ONCE */
    wd = inotify_add_watch (notifyFD, filename, IN_CLOSE_WRITE | IN_DELETE_SELF | IN_ONESHOT);
    if (wd < 0) {
        fprintf ( stderr, "inotify_add_watch() failed: %s\n", strerror (errno));
        return 1;
    }

    for (i = 0 ; ; i++) {
        struct pollfd pollfd;
        
	memset(&pollfd, 0, sizeof(pollfd));
        pollfd.fd = notifyFD;
        pollfd.events = POLLIN;
        
        /* create an event on the file */
        makeFile (filename);
	
        if (poll(&pollfd, 1, 5000) == 0) {
    	    /* in case of no bug this is default */
    	    fprintf (stderr, "inotify: no bug detected!\n");
    	    return 0;
        }
        
        ret = read(notifyFD, &event, sizeof(event));
        
        if (ret < 0) {
            fprintf (stderr, "inotify read() failed: %s\n", strerror(errno));
            return 1;
        }
        else if (ret != sizeof(event)) {
            fprintf ( stderr, "inotify read() returned %d not %d\n", ret, (int)sizeof(event));
            return 1;
        }
        else if (event.wd != wd) {
            fprintf ( stderr, "Watch mismatch, expected %d, got %d\n", wd, event.wd);
            return 1;
        }
        else if (i > 1) {
    	    fprintf (stderr, "inotify: bug detected, mask=%x!\n", event.mask);
    	    return 2;
        }

        /* progress report... */
        fprintf ( stderr, " %d : %d  \r", i, wd );
    }

    return 0;
}
