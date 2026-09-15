# The capsule

A beamfs block is a capsule: 4096 bytes that carry, alone, everything
needed to say whether they are sound, repair themselves, prove they are
the block that was asked for, and say when they were written.

    offset        contents              covered by
    0 .. 3807     user data             the sixteen codewords
    3808 .. 3815  csum                  the sixteen codewords
    3816 .. 3823  selfid                the sixteen codewords
    3824 .. 4079  parity, 16 x 16       --
    4080 .. 4087  generation            --
    4088 .. 4095  reserved, zero        --

The first 3824 bytes are interleaved: symbol `i` of codeword `j` sits
at byte `i*16 + j`.

## Why interleaved

A heavy ion through a die corrupts neighbouring cells, not scattered
ones. The number that matters is therefore how many *consecutive* bytes
a block survives, not how many symbols the code corrects.

Laid out contiguously, codeword `j` owns bytes `[j*239, (j+1)*239)`.
Nine consecutive flips are nine symbols in one codeword, which corrects
eight, and the codeword is lost. That is one ion track.

Interleaved, those nine are one symbol in each of nine different
codewords. It takes 139 consecutive bytes to lose one.

Measured three ways that agree:

- `tools/fsck.beamfs/tests/interleave_test.c`, the codec in userspace
- `/sys/kernel/debug/beamfs/rs_bench`, the kernel's own codec:
  `BURST_BYTES_CONTIGUOUS=8`, `BURST_BYTES_INTERLEAVED=139`
- `tools/fsck.beamfs/tests/fsck_oracle.c`, the checker's verdict on a
  real volume

It costs no extra parity and no measurable time: -1.3% of encode, which
is noise.

## Why the header is inside

The csum says whether a block is sound. The selfid says whether it is
the block that was asked for -- a corrupted pointer that lands on
another valid block returns that block's contents, and every integrity
check passes, because the block is intact; it is simply not the right
one. Measured 2026-08-19: an inode's direct pointer went from 471 to
503 and `cat` exited 0 with 15245 wrong bits.

Both sit outside every codeword today. Nine bytes there condemn a block
whose data is untouched, or make a block refuse to identify itself.
A capsule that cannot protect its own header is not a unit of survival.

Moving them inside costs sixteen bytes of capacity per block -- 3808
rather than 3824, 0.42%, four megabytes on a gigabyte -- and gives the
header the same 139-byte burst resistance as the data.

`tests/capsule_test.c` checks it, including a burst straddling the
boundary between data and header: an ion track does not stop at a
logical edge.

## Why the generation is outside

A generation needs no correction. A stale one is caught by disagreeing
with what the tree expects, not by being decoded, and a generation that
cannot be read at all is as good as one that disagrees.

It closes the gap behind every LOST POINTER seen in a sweep: parity
describing what a block used to hold, with nothing on the block to say
so. The region keeps a signature per block and a freed block leaves it
behind; the next owner inherits a description of the previous one's
contents and `verify` reports the new block as corrupt against it.

## What this does not do

An inode is 172 bytes under one codeword. There is nothing to
interleave, and nine consecutive bytes lose it -- and with it a whole
file. Splitting it into two codewords takes the fatal burst from 9 to
10, into four from 9 to 12: an inode is too small for interleaving to
work on.

What would protect one is a replica. See `burst-resistance.md`.

## Which blocks are capsules

Only the ones holding file data.

| block | layout | payload |
|---|---|---|
| file data | capsule, interleaved | 3808 |
| directory | alternating | 3824 |
| indirect | alternating | 3824 |
| bitmap | alternating | 3824 |
| inode table | one codeword per inode | 172 per inode |

The gain is on file data, which is what a volume mostly holds and what
a burst mostly hits. The others keep their layout, and with it their
payload: `beamfs_block_payload()` answers for a data block, and a
directory block is 3824 whatever the volume's data blocks do.

Getting that backwards has a specific shape. A reader seeking by 3808
into directory blocks laid out in 3824 finds every entry past the first
block sixteen bytes off, and nothing says so -- the checksums are over
the block, not over where the reader thought an entry started.

## Compatibility

Feature bit 18, `BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE`. Reading an
interleaved block with the contiguous arithmetic gathers the wrong
symbols and decodes to noise, so a kernel without it refuses the mount
rather than returning that.

## Measured on a live volume

2026-09-15, x86-01, kernel 7.3.0-rc2, 1 GiB volume.

A 100 kB file written, the volume unmounted, nine consecutive bytes
flipped in its first data block, remounted, and the file read back:

    ordinary      the file is unreadable
    interleaved   the file is byte-for-byte what it was

The same nine bytes. One is a codeword past correction; the other is
one symbol in each of nine codewords.

And before that, the plain question -- does the filesystem return what
it was given: eight megabytes written to each layout, unmounted,
remounted, identical both times, with fsck clean on both.
