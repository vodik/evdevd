#ifndef EVDEVD_H
#define EVDEVD_H

#include <linux/input.h>

struct command_t {
    const char *name;
    char *const *argv;
} Keys[] = {
    [KEY_VOLUMEUP] = {
        .name = "ponymix",
        .argv = (char *const []){ "/usr/bin/ponymix", "increase", "5", NULL }
    },
    [KEY_VOLUMEDOWN] = {
        .name = "ponymix",
        .argv = (char *const []){ "/usr/bin/ponymix", "decrease", "5", NULL }
    }
};

#endif
