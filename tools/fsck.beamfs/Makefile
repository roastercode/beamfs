# SPDX-License-Identifier: GPL-2.0-only
#
# fsck.beamfs - Offline filesystem checker for beamfs
#
# Standalone build: depends only on libc. No external libraries.
# Reed-Solomon decoder copy (single-file, GPL-2.0) is introduced in
# sub-phase 2 (Pass 1 superblock check). For sub-phase 1 (skeleton)
# the build target is just main.c + stubs.
#

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE
LDFLAGS ?=

PREFIX     ?= /usr
SBINDIR    ?= $(PREFIX)/sbin
MANDIR     ?= $(PREFIX)/share/man
INSTALL    ?= install

BIN     := fsck.beamfs
SRCS    := fsck.beamfs.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJS)

install: $(BIN)
	$(INSTALL) -d $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
