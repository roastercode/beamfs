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
SRCS    := fsck.beamfs.c crc32.c rs_decode.c fsck_read.c fsck_pass6.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all clean install test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJS) tests/test_rs_vectors tests/test_rs_vectors.o

install: $(BIN)
	$(INSTALL) -d $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
	$(INSTALL) -d $(DESTDIR)$(MANDIR)/man8
	$(INSTALL) -m 0644 $(BIN).8 $(DESTDIR)$(MANDIR)/man8/$(BIN).8

# Unit tests. 'make test' builds and runs tests/test_rs_vectors.
# Exit 77 is the autotools 'skipped' convention; honoured here while
# the rs_decode body is a skeleton (sub-2.partial-2).
tests/test_rs_vectors: tests/test_rs_vectors.o rs_decode.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/test_rs_vectors.o: tests/test_rs_vectors.c rs_decode.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<

.PHONY: test
test: tests/test_rs_vectors
	@./tests/test_rs_vectors; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "OK"; \
	elif [ $$rc -eq 77 ]; then echo "SKIPPED (decoder skeleton)"; \
	else echo "FAIL rc=$$rc"; exit $$rc; fi
