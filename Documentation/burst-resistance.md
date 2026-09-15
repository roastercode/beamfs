# What a burst destroys, by structure

A heavy ion through a die corrupts neighbouring cells, not scattered
ones. The number that matters for beamfs is therefore not "how many
symbols can it correct" but "how many consecutive bytes can it lose".

Measured three ways that agree: the codec in userspace
(`tools/fsck.beamfs/tests/interleave_test.c`), the kernel's own codec
(`/sys/kernel/debug/beamfs/rs_bench`), and the checker's verdict on a
real volume (`tests/fsck_oracle.c`).

| structure | data | parity | codewords | fatal burst | what is lost |
|---|---|---|---|---|---|
| data block | 239 x 16 | 16 x 16 | 16 | 9 bytes | 3824 bytes |
| indirect block | 239 x 16 | 16 x 16 | 16 | 9 bytes | 512 pointers |
| bitmap block | 239 x 16 | 16 x 16 | 16 | 9 bytes | the state of 1912 blocks |
| inode | 172 | 16 | 1 | 9 bytes | one whole file |

Nine bytes. One ion track.

## What interleaving fixes

Contiguously, codeword j owns bytes [j*239, (j+1)*239), so nine
consecutive flips are nine symbols in one codeword and it corrects
eight. Interleaved -- symbol i of codeword j at byte i*16 + j -- the
same nine are one symbol in each of nine codewords.

| structure | fatal burst, today | interleaved |
|---|---|---|
| data block | 9 | 139 |
| indirect block | 9 | 139 |
| bitmap block | 9 | 139 |
| inode | 9 | **9** |

Measured, not derived: `BURST_BYTES_CONTIGUOUS=8` and
`BURST_BYTES_INTERLEAVED=139` from the kernel's codec.

For no extra parity and no measurable time: -1.3% of encode, which is
noise.

## What it does not fix

An inode is 172 bytes under one codeword. There is nothing to
interleave: eight symbols anywhere, and the ninth loses it.

Splitting it does not help much. Two codewords of 86 data bytes with 8
parity each takes the fatal burst from 9 to 10; four of 43 with 4 each,
to 12. An inode is too small for interleaving to work on.

What would protect one is a copy. An inode is 256 bytes and a block
holds sixteen; a replica elsewhere costs half the inode table and makes
a burst irrelevant -- the reader takes whichever decodes. That is a
different decision about the format, with a different cost, and it
belongs in its own study rather than being folded into this one.

## Why this matters here

beamfs exists to survive radiation. The structure whose loss costs most
-- an inode, and with it a whole file -- is the one interleaving cannot
help, and that has to be said plainly rather than left implicit behind
a table of averages.
