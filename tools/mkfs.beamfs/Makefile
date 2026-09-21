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
RS      := ../fsck.beamfs

# Every warning on, and treated as an error.
#
# -Wformat=2 first: a %lu given an unsigned long long compiles clean
# where the types happen to match and breaks where they do not.
#
# -Wconversion without -Wsign-conversion: the sign variant fires on
# Galois-field arithmetic whose values are bounded by construction, and
# a flag that shouts on correct code is one that gets turned off.
CFLAGS  ?= -O2 -std=gnu17 -D_GNU_SOURCE \
	   -Wall -Wextra -Wformat=2 -Wconversion -Wno-sign-conversion \
	   -Wcast-qual -Wpointer-arith -Wmissing-prototypes -Wundef \
	   -Wstrict-prototypes -Wshadow \
	   -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
	   -Werror
LDFLAGS ?=

PREFIX  ?= /usr
SBINDIR ?= $(PREFIX)/sbin
INSTALL ?= install

BIN  := mkfs.beamfs

.PHONY: all clean install

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
