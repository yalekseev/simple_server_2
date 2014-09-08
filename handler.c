#include "io.h"
#include "handler.h"

#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>

enum { NUM_PROCS = 12 };

static pid_t procs[NUM_PROCS];

static void handle_single_request(int socket_fd) {
    /* set send/receive timeouts */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        return;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
        return;
    }

    char file_name[MAX_FILE_NAME + 1];
    ssize_t bytes_read = readn(socket_fd, file_name, MAX_FILE_NAME);
    if (bytes_read <= 0) {
        return;
    }

    file_name[bytes_read] = '\0';

    int file_fd = open(file_name, O_RDONLY);
    if (-1 == file_fd) {
        if (errno != ENOENT) {
            syslog(LOG_ERR, "open %s", strerror(errno));
        }
        return;
    }

    if (-1 == sendfile(socket_fd, file_fd, NULL, MAX_FILE_SIZE)) {
        syslog(LOG_ERR, "sendfile %s", strerror(errno));
    }

    if (-1 == close(file_fd)) {
        syslog(LOG_ERR, "close %s", strerror(errno));
    }
}

static void handle_requests(int server_fd) {
    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (-1 == client_fd && errno == EINTR) {
            continue;
        } else if (-1 == client_fd && errno == ECONNABORTED) {
            continue;
        } else if (-1 == client_fd) {
            syslog(LOG_ERR, "accept %s", strerror(errno));
            continue;
        }

        handle_single_request(client_fd);

        if (-1 == close(client_fd)) {
            syslog(LOG_ERR, "close %s", strerror(errno));
        }
    }
}

static void terminate(int signo) {
    int i;
    for (i = 0; i < NUM_PROCS; ++i) {
        kill(procs[i], SIGTERM);
    }

    while (wait(NULL) > 0) {
        ;
    }

    _exit(0);
}

void spawn_service_tasks(int server_fd) {
    struct sigaction sa;
    sa.sa_handler = &terminate;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        syslog(LOG_EMERG, "sigaction %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_EMERG, "sigaction %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int i;
    for (i = 0; i < NUM_PROCS; ++i) {
        pid_t pid;
        pid = fork();

        if (pid == 0) {
            handle_requests(server_fd);
        } else if (pid > 0) {
            procs[i] = pid;
        } else {
            terminate();
        }
    }
}
