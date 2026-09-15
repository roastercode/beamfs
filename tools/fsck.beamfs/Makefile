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
# Every warning on, and treated as an error.
#
# -Wformat=2 first: a %lu given an unsigned long long compiles clean
# where the types happen to match and breaks where they do not, and two
# of those reached a commit before this line existed.
#
# -Wconversion without -Wsign-conversion: the sign variant fires
# nineteen times in rs_decode.c, on Galois-field arithmetic whose
# values are bounded by construction, and a flag that shouts on correct
# code is one that gets turned off. The truncation half is what catches
# an offset losing its top bits, which is the mistake that matters.
CFLAGS  ?= -O2 -std=gnu11 -D_GNU_SOURCE \
	   -Wall -Wextra -Wformat=2 -Wconversion -Wno-sign-conversion \
	   -Wcast-qual -Wpointer-arith -Wmissing-prototypes -Wundef \
	   -Wstrict-prototypes -Wshadow \
	   -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
	   -Werror
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

# What a parity region survives when one slot is edited.
#
# A function doing read-modify-write on a region went onto the free
# path untested and produced 56 "beyond correction" messages in one
# sweep. These are the questions that should have come first.
# What interleaving buys against bursts.
#
# A heavy ion corrupts neighbouring cells, so today's nine-byte burst
# lands in one codeword and takes it past correction. Interleaved it
# takes 129, for no extra parity.
# The failing test: a nine-byte burst on a block as laid out today.
tests/burst_test: tests/burst_test.o rs_decode.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/burst_test.o: tests/burst_test.c rs_decode.h beamfs_format.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<

tests/interleave_test: tests/interleave_test.o rs_decode.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/interleave_test.o: tests/interleave_test.c rs_decode.h beamfs_format.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<

tests/region_test: tests/region_test.o rs_decode.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/region_test.o: tests/region_test.c rs_decode.h beamfs_format.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<


tests/test_rs_vectors.o: tests/test_rs_vectors.c rs_decode.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<

.PHONY: test
test: tests/test_rs_vectors
	@./tests/test_rs_vectors; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "OK"; \
	elif [ $$rc -eq 77 ]; then echo "SKIPPED (decoder skeleton)"; \
	else echo "FAIL rc=$$rc"; exit $$rc; fi
	./tests/region_test
	./tests/interleave_test
