# SPDX-License-Identifier: GPL-2.0-only
#
# mkfs.beamfs - format a block device or image as beamfs
#
# The Reed-Solomon decoder is the checker's, not a second copy.
#
# A second RS implementation diverging from the first already cost this
# project an incident, and the parity region has to be decoded before a
# slot in it can be edited, so the formatter needs a decoder that agrees
# with the kernel's byte for byte. One file, one behaviour.

CC      ?= gcc
CFLAGS  ?= -O2
LDFLAGS ?=

# The checker's directory, where the decoder is. A caller that has the
# decoder beside the formatter, as the Yocto recipe does, passes RS=.
RS      := ../fsck.beamfs

# Every warning on, and treated as an error, whatever CFLAGS holds:
# added with override, after CFLAGS, so that a caller's CFLAGS cannot
# take them away. The list is fsck.beamfs's, whose Makefile says why.
WARNINGS := -Wall -Wextra -Wformat=2 -Werror=format -Wshadow \
	    -Wmissing-prototypes -Wmissing-declarations -Wundef \
	    -Wstrict-prototypes -Wno-unused-parameter \
	    -Wconversion -Wsign-conversion -Wcast-qual -Wpointer-arith \
	    -D_FORTIFY_SOURCE=2 -fstack-protector-strong -Werror
ANALYZER := $(shell $(CC) -fanalyzer -c -x c /dev/null -o /dev/null \
	    2>/dev/null && echo -fanalyzer)
override CFLAGS += -std=gnu17 -D_GNU_SOURCE $(WARNINGS) $(ANALYZER)

PREFIX  ?= /usr
SBINDIR ?= $(PREFIX)/sbin
INSTALL ?= install

BIN  := mkfs.beamfs

.PHONY: all clean install

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
	@rm -f $(TMPDIR)/mkfs-vg.img && truncate -s 1G $(TMPDIR)/mkfs-vg.img
	valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all \
	  --errors-for-leak-kinds=all --track-origins=yes -q \
	  ./$(BIN) -N 16384 $(TMPDIR)/mkfs-vg.img >/dev/null
	@rm -f $(TMPDIR)/mkfs-vg.img
	@echo "check-valgrind: mkfs.beamfs clean"

all: $(BIN)

$(BIN): mkfs.beamfs.o rs_decode.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BIN) mkfs.beamfs.o rs_decode.o

mkfs.beamfs.o: mkfs.beamfs.c
	$(CC) $(CFLAGS) -I. -I$(RS) -c -o mkfs.beamfs.o mkfs.beamfs.c

rs_decode.o: $(RS)/rs_decode.c
	$(CC) $(CFLAGS) -I$(RS) -c -o rs_decode.o $(RS)/rs_decode.c

clean:
	rm -f $(BIN) mkfs.beamfs.o rs_decode.o

install: $(BIN)
	$(INSTALL) -d $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
