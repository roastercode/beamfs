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
CFLAGS  ?= -O2
LDFLAGS ?=

# Every warning on, and treated as an error, whatever CFLAGS holds.
#
# They are added with override, after CFLAGS. Set with ?= next to -O2,
# as they were until 0.1.8, they went as soon as a caller passed CFLAGS
# of its own: the Yocto recipe does, and the fsck.beamfs of the nodes
# was built with Yocto's flags, -Wall and -Wextra, and no -Werror.
#
# -Werror=format first: a %lu given an unsigned long long compiles clean
# where the types happen to match and breaks where they do not, and two
# of those reached a commit before this line existed.
#
# -Wsign-conversion too: the Galois-field arithmetic it was turned off
# for is bounded by construction, and a cast says so where it happens.
# -fanalyzer when the compiler has it.
WARNINGS := -Wall -Wextra -Wformat=2 -Werror=format -Wshadow \
	    -Wmissing-prototypes -Wmissing-declarations -Wundef \
	    -Wstrict-prototypes -Wno-unused-parameter \
	    -Wconversion -Wsign-conversion -Wcast-qual -Wpointer-arith \
	    -D_FORTIFY_SOURCE=2 -fstack-protector-strong -Werror
ANALYZER := $(shell $(CC) -fanalyzer -c -x c /dev/null -o /dev/null \
	    2>/dev/null && echo -fanalyzer)
override CFLAGS += -std=gnu11 -D_GNU_SOURCE $(WARNINGS) $(ANALYZER)

PREFIX     ?= /usr
SBINDIR    ?= $(PREFIX)/sbin
MANDIR     ?= $(PREFIX)/share/man
INSTALL    ?= install

BIN     := fsck.beamfs
SRCS    := fsck.beamfs.c crc32.c rs_decode.c fsck_read.c fsck_pass6.c
OBJS    := $(SRCS:.c=.o)
TESTS   := tests/test_rs_vectors tests/region_test tests/interleave_test \
	   tests/capsule_test tests/burst_test tests/fsck_oracle

.PHONY: all clean install test

# Whatever comes first below, a bare make builds the tool: bitbake
# calls oe_runmake without a target, and on 2026-09-23 it ran
# check-valgrind on the cross target instead of compiling.
.DEFAULT_GOAL := all

# Under Valgrind, against a 1 GiB image in $(TMPDIR). Every byte lost
# is an error: e2fsprogs has held that bar for twenty years, and the
# one leak fsck.beamfs carried into 0.1.5 sat on the path every healthy
# volume takes. Needs valgrind on the host; skipped with 77 without it.
TMPDIR ?= /tmp
.PHONY: check-valgrind
check-valgrind: $(BIN)
	@command -v valgrind >/dev/null 2>&1 || { echo "check-valgrind: no valgrind"; exit 77; }
	@rm -f $(TMPDIR)/fsck-vg.img && truncate -s 1G $(TMPDIR)/fsck-vg.img
	@$(MAKE) -s -C ../mkfs.beamfs all >/dev/null
	@../mkfs.beamfs/mkfs.beamfs -N 16384 $(TMPDIR)/fsck-vg.img >/dev/null 2>&1
	valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
	  --errors-for-leak-kinds=all --track-origins=yes -q \
	  ./$(BIN) -n $(TMPDIR)/fsck-vg.img >/dev/null
	valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
	  --errors-for-leak-kinds=all --track-origins=yes -q \
	  ./$(BIN) -n -v $(TMPDIR)/fsck-vg.img >/dev/null
	@rm -f $(TMPDIR)/fsck-vg.img
	@echo "check-valgrind: fsck.beamfs clean"

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJS) $(TESTS) $(TESTS:=.o)

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
# The oracle: mkfs a volume, damage it, and see what the checker says.
tests/fsck_oracle: tests/fsck_oracle.c rs_decode.o crc32.o beamfs_format.h rs_decode.h
	$(CC) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/fsck_oracle.c rs_decode.o crc32.o

# A capsule, and what it survives -- the header included.
tests/capsule_test: tests/capsule_test.o rs_decode.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tests/capsule_test.o: tests/capsule_test.c rs_decode.h beamfs_format.h
	$(CC) $(CFLAGS) -I. -c -o $@ $<

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

# test builds every test it runs. It depended on test_rs_vectors alone
# and ran region_test and interleave_test as well, which a tree where
# nothing else had built them did not have.
.PHONY: test
test: tests/test_rs_vectors tests/region_test tests/interleave_test
	@./tests/test_rs_vectors; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "OK"; \
	elif [ $$rc -eq 77 ]; then echo "SKIPPED (decoder skeleton)"; \
	else echo "FAIL rc=$$rc"; exit $$rc; fi
	./tests/region_test
	./tests/interleave_test
