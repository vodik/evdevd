#include "evdevd.h"

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

static bool sd_activated = false;
static struct udev *udev;
static struct udev_monitor *input_mon;
static int epoll_fd, server_sock;

int last_cfd = 0;

size_t init_evdevd_socket(struct sockaddr_un *un);
void unlink_evdevd_socket(void);

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

// {{{1 EVDEV
static inline uint8_t bit(int bit, const uint8_t array[static (EV_MAX + 7) / 8])
{
    return array[bit / 8] & (1 << (bit % 8));
}

static int ev_open(const char *devnode, const char *name, size_t buf)
{
    int fd, rc = 0;
    uint8_t evtype_bitmask[(EV_MAX + 7) / 8];

    fd = open(devnode, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -1;

    rc = ioctl(fd, EVIOCGBIT(0, EV_MAX), evtype_bitmask);
    if (rc < 0)
        goto cleanup;

    if (!bit(EV_KEY, evtype_bitmask))
        goto cleanup;

    if (buf)
        rc = ioctl(fd, EVIOCGNAME(buf), name);

cleanup:
    if (rc <= 0) {
        close(fd);
        return -1;
    }
    return fd;
}
// }}}

static void udev_adddevice(struct udev_device *dev, bool enumerating)
{
    const char *devnode = udev_device_get_devnode(dev);
    const char *action = udev_device_get_action(dev);
    const char name[256];

    /* check there's an entry in /dev/... */
    if (!devnode)
        return;

    /* if we aren't enumerating, check there's an action */
    if (!enumerating && !action)
        return;

    /* check if device has ID_INPUT_keyboard */
    if (udev_device_get_property_value(dev, "ID_INPUT_KEY") == NULL)
        return;

    if (enumerating || strcmp("add", action) == 0) {
        int fd = ev_open(devnode, name, sizeof(name));
        if (fd < 0)
            return;

        printf("Monitoring device %s%s: %s\n", name, enumerating ? "[enumerated]" : "", devnode);

        struct epoll_event event = {
            .data.fd = fd,
            .events  = EPOLLIN | EPOLLET
        };

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)
            err(EXIT_FAILURE, "failed to add device to epoll");

        /* register_device(devnode, fd); */
        /* register_epoll(fd, power_mode); */
    } else if (strcmp("remove", action) == 0) {
        /* unregister_device(devnode); */
    }
}

static void udev_init_input(void)
{
    struct udev_list_entry *devices, *dev_list_entry;
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);

    input_mon = udev_monitor_new_from_netlink(udev, "udev");
    udev_monitor_filter_add_match_subsystem_devtype(input_mon, "input", NULL);
    udev_monitor_enable_receiving(input_mon);

    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);

    devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(dev_list_entry, devices) {
        const char *path = udev_list_entry_get_name(dev_list_entry);
        struct udev_device *dev = udev_device_new_from_syspath(udev, path);

        udev_adddevice(dev, true);
        udev_device_unref(dev);
    }

    udev_enumerate_unref(enumerate);
}

static void udev_monitor_input(void)
{
    while (true) {
        struct udev_device *dev = udev_monitor_receive_device(input_mon);
        if (!dev) {
            if (errno == EAGAIN)
                break;
            err(EXIT_FAILURE, "failed to recieve input device");
        }

        udev_adddevice(dev, false);
        udev_device_unref(dev);
    }
}

/* {{{1 SOCKETS */
static int get_socket(void)
{
    int fd, n;

    n = sd_listen_fds(0);
    if (n > 1)
        err(EXIT_FAILURE, "too many file descriptors recieved");
    else if (n == 1) {
        fd = SD_LISTEN_FDS_START;
        sd_activated = true;
    } else {
        union {
            struct sockaddr sa;
            struct sockaddr_un un;
        } sa;
        socklen_t sa_len;

        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            err(EXIT_FAILURE, "couldn't create socket");

        sa_len = init_evdevd_socket(&sa.un);
        if (bind(fd, &sa.sa, sa_len) < 0)
            err(EXIT_FAILURE, "failed to bind");

        if (sa.un.sun_path[0] != '@')
            /* chmod(sa.un.sun_path, multiuser_mode ? 0777 : 0700); */
            chmod(sa.un.sun_path, 0777);

        if (listen(fd, SOMAXCONN) < 0)
            err(EXIT_FAILURE, "failed to listen");
    }

    return fd;
}
/* }}} */

/* {{{1 CLIENT CODE */
static void read_event(int fd)
{
    ssize_t nbytes_r;
    struct input_event event;

    while (true) {
        nbytes_r = read(fd, &event, sizeof(struct input_event));
        if (nbytes_r < 0) {
            if (errno == EAGAIN)
                return;
            err(EXIT_FAILURE, "failed to read input events");
        }

        /* if (event.type != EV_KEY) */
        /*     return; */

        if (event.value != 1)
            continue;

        if (last_cfd) {
            printf("%d, %d, %d\n", event.type, event.code, event.value);
            write(last_cfd, &event, sizeof(struct input_event));
        }
    }
}

static void accept_conn()
{
    int cfd = accept4(server_sock, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0)
        err(EXIT_FAILURE, "failed to accept connection");

    last_cfd = cfd;
}

static int loop(void)
{
    int udev_mon_fd = udev_monitor_get_fd(input_mon);
    struct epoll_event events[4], udev_event = {
        .data.fd = udev_mon_fd,
        .events = EPOLLIN | EPOLLET
    }, socket_event = {
        .data.fd = server_sock,
        .events = EPOLLIN | EPOLLET
    };

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udev_mon_fd, &udev_event) < 0)
        err(EXIT_FAILURE, "failed to add udev to epoll");
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &socket_event) < 0)
        err(EXIT_FAILURE, "failed to add socket to epoll");

    while (true) {
        int i, n = epoll_wait(epoll_fd, events, 4, -1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            err(EXIT_FAILURE, "epoll_wait failed");
        }

        for (i = 0; i < n; ++i) {
            struct epoll_event *evt = &events[i];

            if (evt->events & EPOLLERR || evt->events & EPOLLHUP)
                close(evt->data.fd);
            else if (evt->data.fd == server_sock)
                accept_conn();
            else if (evt->data.fd == udev_mon_fd)
                udev_monitor_input();
            else
                read_event(evt->data.fd);
        }
    }

    return 0;
}

int main(void)
{
    udev = udev_new();
    if (!udev)
        err(EXIT_FAILURE, "can't create udev");

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        err(EXIT_FAILURE, "failed to create epoll fd");

    udev_init_input();
    server_sock = get_socket();

    loop();
}
