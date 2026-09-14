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

For `fsck.beamfs` and `mkfs.beamfs`:

    -std=gnu17
    -Wall -Wextra -Wformat=2 -Wconversion -Wno-sign-conversion
    -Wcast-qual -Wpointer-arith -Wmissing-prototypes -Wundef
    -Wstrict-prototypes -Wshadow
    -D_FORTIFY_SOURCE=2 -fstack-protector-strong
    -Werror

`gnu17` rather than `gnu11`: the same diagnostics, six years newer, and
none of C23's breaking changes. `gnu23` was measured and gives the same
count, but it is what broke flex 2.6.4 on this host in September --
empty parameter lists changed meaning -- and there is no reason to take
that on for nothing.

`-Wno-sign-conversion` because `-Wconversion` implies it in C, and the
sign half fires nineteen times in `rs_decode.c` alone, on Galois-field
arithmetic whose values are bounded by the field. Those are correct and
the flag is wrong about them; the truncation half is what catches an
offset losing its top bits.

`-Wshadow` is on here, where the headers are ours, and it found a real
redundancy: `inodes_per_block` declared twice in `mkfs.beamfs.c`,
computed the same way both times.

## What a local build does not prove

`make` on the workstation compiles the module against the workstation's
kernel headers. A function that exists only on the target path is never
compiled, a structure that changed between versions is the local one,
and a signature that differs -- `sync_inode_metadata` gained a
`writeback_control` in 7.3 -- fails here and passes there, or the
reverse.

`bitbake` is the only build that counts. A commit proposed without it is
the same mistake as a patch sent without a test.
