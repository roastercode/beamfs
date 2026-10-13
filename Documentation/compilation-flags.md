# Compilation flags

Nothing here builds with the defaults.

## Why

Two errors reached a commit on 2026-09-14 that the host compiler did not
see and the target compiler stopped on:

    file_inline.c: error: format '%lu' expects argument of type
        'long unsigned int', but argument 3 has type
        'long long unsigned int'

    file_inline.c: error: 'sb' undeclared in this function

The first is what `-Werror=format` exists for. `inode->i_ino` is an
`unsigned long` and the fallback beside it was written `0UL`; on the
host the two happened to agree and on the target they did not.

The second no flag catches. It is a compilation error in a function the
host build never compiles, because the path it lives on is not taken
there. Only `bitbake` sees it, which is why a local `make` is never the
last word.

## Kernel module

In `ccflags-y`:

    -Wall -Wextra -Wformat=2 -Werror=format
    -Wmissing-prototypes -Wmissing-declarations
    -Wundef -Wstrict-prototypes
    -Wno-unused-parameter
    -Werror

`-Wshadow` is deliberately absent. The kernel's own headers trip it --
`cc_mask` in `asm/text-patching.h` shadows a global -- and a flag that
fires on somebody else's code is a flag that gets turned off.

`-Wno-unused-parameter` because the kernel hands most handlers more
arguments than they use.

The standard is the kernel's, currently `gnu11`. That is not a choice
the module gets to make.

## Userspace tools

For `fsck.beamfs` and `mkfs.beamfs`, in their Makefiles:

    -Wall -Wextra -Wformat=2 -Werror=format -Wshadow
    -Wmissing-prototypes -Wmissing-declarations -Wundef
    -Wstrict-prototypes -Wno-unused-parameter
    -Wconversion -Wsign-conversion -Wcast-qual -Wpointer-arith
    -D_FORTIFY_SOURCE=2 -fstack-protector-strong
    -Werror

and `-fanalyzer` when the compiler accepts it. `fsck.beamfs` is built
with `-std=gnu11`, `mkfs.beamfs` with `-std=gnu17`.

The list is added with `override CFLAGS +=`, after whatever `CFLAGS`
the caller passes; `CFLAGS` itself defaults to `-O2` and nothing else.
Until fsck.beamfs 0.1.9 and mkfs.beamfs 0.1.6 the list was the default
value of `CFLAGS`, and a caller that passed its own replaced it. The
Yocto recipe of fsck.beamfs does pass its own, and the checker on the
nodes was built with Yocto's flags, `-Wall` and `-Wextra`, and without
`-Werror`. The recipes now pass Yocto's flags and nothing more.

`gnu17` rather than `gnu11` for mkfs: the same diagnostics, six years
newer, and none of C23's breaking changes. `gnu23` was measured and
gives the same count, but it is what broke flex 2.6.4 on this host in
September, where empty parameter lists changed meaning, and there is
no reason to take that on for nothing.

`-Wsign-conversion` is on since fsck.beamfs 0.1.9 and mkfs.beamfs
0.1.6. It was off because it fired on Galois-field arithmetic in
`rs_decode.c` whose values are bounded by the field; it fired because
the bound was not written there. Each place it fired now spells out
the conversion C already made, where C made it. Compared with
`objdump -d -r`, assertions compiled out since lines moved, the machine
code of mkfs.beamfs, of the decoder and of the CRC is the same as
before. A first attempt that moved conversions instead (a parameter's
type, a helper function, a variable's type) changed it, though not the
values: the compiler used the freedom it had been given.

One computation was rewritten rather than spelled out: an inode's byte
offset on the medium, in pass 3 of fsck.beamfs, in its reader and in
its test oracle. It multiplied the block number as a signed value and
added the offset in the block as an unsigned one, so the sum was
unsigned and converted back. It is unsigned throughout now, converted
once. The compiler factors it, `(block * 16 + offset) << 8` where it
computed `(block << 12) + (offset << 8)`: the same value wherever the
old expression was defined, which its signed product is not past 2^51
blocks. The test oracle gives the same verdicts with the old tools and
the new ones.

`-Wshadow` is on here, where the headers are ours, and it found a real
redundancy: `inodes_per_block` declared twice in `mkfs.beamfs.c`,
computed the same way both times.

## tools/check-build.sh

`make` here compiles two files out of sixteen: it builds against the
workstation's kernel, super.c fails on a signature that changed in 7.3,
and everything after it is never reached. Twice in one morning that
silence read as a pass.

`tools/check-build.sh` copies the sources into the tree Yocto unpacked
and asks kbuild to build the one directory, with the config, compiler
and headers the real build uses. Sixteen files, thirty seconds, against
bitbake's twenty minutes.

It runs before a commit is proposed. It does not replace bitbake: link
time, the recipe's Kconfig lines and the image itself only happen there.

## What a local build does not prove

`make` on the workstation compiles the module against the workstation's
kernel headers. A function that exists only on the target path is never
compiled, a structure that changed between versions is the local one,
and a signature that differs -- `sync_inode_metadata` gained a
`writeback_control` in 7.3 -- fails here and passes there, or the
reverse.

`bitbake` is the only build that counts. A commit proposed without it is
the same mistake as a patch sent without a test.
