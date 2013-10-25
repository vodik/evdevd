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
#include <sys/wait.h>
#include <linux/input.h>
#include <libudev.h>

static struct udev *udev;
static struct udev_monitor *input_mon;
static int epoll_fd;

// {{{1 EVDEV
static inline uint8_t bit(int bit, const uint8_t array[static (EV_MAX + 7) / 8])
{
    return array[bit / 8] & (1 << (bit % 8));
}

static int ev_open(const char *devnode, const char *name, size_t buf)
{
    int fd, rc = 0;
    uint8_t evtype_bitmask[(EV_MAX + 7) / 8];

    fd = open(devnode, O_RDONLY | O_NONBLOCK);
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

        printf("%d, %d, %d\n", event.type, event.code, event.value);

        if (Keys[event.code].name) {
            printf("RUNNING %s\n", Keys[event.code].name);
            run_command(&Keys[event.code]);
        }
    }
}

static int loop(void)
{
    int udev_mon_fd = udev_monitor_get_fd(input_mon);
    struct epoll_event events[4], event = {
        .data.fd = udev_mon_fd,
        .events = EPOLLIN | EPOLLET
    };

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udev_mon_fd, &event) < 0)
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
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        err(EXIT_FAILURE, "failed to create epoll fd");

    udev = udev_new();
    if (!udev)
        err(EXIT_FAILURE, "can't create udev");

    udev_init_input();
    loop();
}
