#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <libudev.h>
#include <systemd/sd-daemon.h>

#include "evdevd.h"

/* {{{1 COMMON */
static const char *get_socket_path(void)
{
    const char *socket = getenv("EVDEVD_SOCKET");
    return socket ? socket : "@/vodik/evdevd";
}

size_t init_evdevd_socket(struct sockaddr_un *un)
{
    const char *socket = get_socket_path();
    off_t off = 0;
    size_t len;

    *un = (struct sockaddr_un){ .sun_family = AF_UNIX };

    if (socket[0] == '@')
        off = 1;

    len = strlen(socket);
    memcpy(&un->sun_path[off], &socket[off], len - off);

    return len + sizeof(un->sun_family);
}

void unlink_evdevd_socket(void)
{
    const char *socket = get_socket_path();
    if (socket[0] != '@')
        unlink(socket);
}
/* }}} */

int get_socket()
{
    socklen_t sa_len;
    union {
        struct sockaddr sa;
        struct sockaddr_un un;
    } sa;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    sa_len = init_evdevd_socket(&sa.un);
    if (connect(fd, &sa.sa, sa_len) < 0)
        return -errno;

    return fd;
}

static void run_command(struct command_t *cmd)
{
    int stat = 0;

    switch (fork()) {
        case -1:
            err(1, "fork failed");
            break;
        case 0:
            execvp(cmd->argv[0], cmd->argv);
            err(EXIT_FAILURE, "failed to start %s", cmd->name);
            break;
        default:
            break;
    }

    if (wait(&stat) < 1)
        err(EXIT_FAILURE, "failed to get process status");

    if (stat) {
        if (WIFEXITED(stat))
            fprintf(stderr, "%s exited with status %d.\n",
                    cmd->name, WEXITSTATUS(stat));
        if (WIFSIGNALED(stat))
            fprintf(stderr, "%s terminated with signal %d.\n",
                    cmd->name, WTERMSIG(stat));
    }
}

int main(void)
{
    ssize_t nbytes_r;
    struct input_event event;
    int fd = get_socket();

    if (fd < 0)
        err(1, "failed to open socket");

    while (true) {
        nbytes_r = read(fd, &event, sizeof(struct input_event));
        if (nbytes_r < 0)
            err(EXIT_FAILURE, "failed to read input events");
        if (nbytes_r == 0)
            return 0;

        /* if (event.type != EV_KEY) */
        /*     return; */

        if (event.value != 1)
            continue;

        printf("%d, %d, %d\n", event.type, event.code, event.value);

        if (Keys[event.code].name) {
            printf("RUNNING %s\n", Keys[event.code].name);
            run_command(&Keys[event.code]);
        }
    }
}
