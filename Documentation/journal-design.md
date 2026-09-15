# A metadata journal for beamfs

## The problem, stated once

design.md says "No journaling: crash consistency relies on
mark_buffer_dirty() ordering". mark_buffer_dirty establishes no
ordering: it marks a buffer dirty and writeback decides when, sorting
by age and by position on the device.

A write through triple indirection touches nine objects -- the bitmap,
the data block, three indirect blocks, their three parity slots, and
the inode. There are 362880 orders in which those can reach the medium
and exactly one is safe: pointee before pointer, always.

723 of 734 tests pass because they write little and never meet a bad
permutation. The nine that fail are the nine that write hard:
fsstress at 128 processes, fsx, dd to ENOSPC. They meet all of them.

Every symptom seen so far is one permutation of that:

  - a block allocated, its bitmap bit written, the pointer never
    reaching the medium: used-but-unreferenced
  - an indirect block whose pointer reached the inode before the block
    itself: never described, subtree unreachable
  - an inode written with its new size before the tree under it:
    claims more blocks than it owns
  - a parity slot written before or after the block it describes:
    beyond correction

## What a journal gives

Write the metadata twice. First to a reserved region, sequentially,
with a marker that says the group is complete. Then to its real place.

A crash leaves one of two states:

  - the marker is absent: the journal is ignored, the volume is as it
    was before the operation
  - the marker is present: recovery replays the journal, the volume is
    as it would have been after

Never a mixture. That is the whole property, and it is what ordering
alone cannot give.

## Structure

### On disk

Two new superblock fields, following the shape s_ind_parity_blk and
s_budget_blk already use:

    __le64  s_journal_blk;   /* first block of the journal */
    __le32  s_journal_len;   /* length, in blocks */

The region is a ring. Each transaction is:

    [descriptor][block][block]...[block][commit]

The descriptor names how many blocks follow and where each belongs.
The commit block carries a sequence number and a CRC over the whole
transaction. A transaction whose commit is missing or whose CRC does
not match is not replayed.

    struct beamfs_jdesc {
        __le32  jd_magic;        /* BEAMFS_JDESC_MAGIC */
        __le32  jd_seq;          /* transaction sequence */
        __le32  jd_count;        /* metadata blocks that follow */
        __le32  jd_crc32;        /* over this descriptor */
        __le64  jd_target[...];  /* where each block belongs */
    };

    struct beamfs_jcommit {
        __le32  jc_magic;        /* BEAMFS_JCOMMIT_MAGIC */
        __le32  jc_seq;          /* must equal the descriptor's */
        __le32  jc_crc32;        /* over every block in the group */
        __le32  jc_pad;
    };

Sizing: a transaction covers one operation's metadata, so nine blocks
is the worst case and sixteen is a comfortable maximum. A journal of
1024 blocks -- 4 MB -- holds sixty of those, which is more than enough
for a filesystem with no delayed allocation.

### In the kernel

    beamfs_trans_begin(sb)        -- start a group
    beamfs_trans_add(t, bh)       -- this buffer belongs to the group
    beamfs_trans_commit(t)        -- write descriptor, blocks, commit;
                                     wait; then let writeback take the
                                     real blocks whenever it likes

The last point is what makes it cheap. Once the transaction is on the
medium, the order of the real writes stops mattering: a crash replays
the journal and puts everything where it belongs.

The ten sites in file_inline.c that install a pointer each become one
transaction. Allocation, the indirect blocks they touch, their parity,
and the inode all go in one group.

### Recovery

At mount, before anything reads the tree:

  - walk the journal from the oldest sequence number
  - for each transaction with a valid commit and CRC, write each block
    to its target
  - stop at the first transaction that does not commit
  - clear the journal

Idempotent by construction: replaying a transaction twice writes the
same bytes.

## Cost

One extra write per operation, sequential and grouped, against up to
four synchronous scattered writes for the ordering alternative. This
is why every journalling filesystem chose it.

The journal region is written in place, over and over, which is worth
saying for a filesystem meant for radiation-prone storage: it is the
hottest region on the volume. The budget region already tracks wear
and would need to count it.

## What this does not fix

Data blocks. A journal of metadata leaves file contents unordered,
which is what ext4's default mode does too -- a crash can leave a file
whose metadata says the block is there and whose block holds the
previous tenant's bytes. Journalling data as well is a mode, not a
default, and beamfs's RS parity over data makes the trade different
enough to deserve its own study.

## Work

The format change and mkfs: small.
The transaction layer: a file, perhaps 400 lines.
The ten call sites: mechanical but every one must be right.
Recovery at mount: small, and testable on its own with a crafted
journal.
fsck: must learn to read and replay the journal, or refuse a volume
whose journal is not empty.

Two to three weeks, against nine tests that will not pass without it.
