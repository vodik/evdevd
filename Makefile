# VERSION = $(shell git describe --tags)

CFLAGS := -std=c99 \
	-Wall -Wextra -pedantic \
	-Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes \
	-D_GNU_SOURCE \
	${CFLAGS}

LDLIBS = -ludev

all: evdevd
evdevd: evdevd.o

clean:
	${RM} evdevd *.o

.PHONY: clean install uninstall
