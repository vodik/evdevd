# VERSION = $(shell git describe --tags)

CFLAGS := -std=c99 \
	-Wall -Wextra -pedantic \
	-Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes \
	-D_GNU_SOURCE \
	${CFLAGS}

LDLIBS = -ludev -lsystemd-daemon

all: evdevd evdev
evdevd: evdevd.o
evdev: evdev.o

clean:
	${RM} evdev evdevd *.o

.PHONY: clean install uninstall
