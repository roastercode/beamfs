/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fsck_read -- one way in from the medium.
 *
 * fsck.beamfs read the device in nine places across five passes, each
 * validating in its own way or not at all. pass 3 decoded an inode
 * whose CRC did not match; pass 4, which builds the reference set and
 * reports used-but-unreferenced, read the same inode raw. The two
 * passes could therefore disagree about what an inode contained, and it
 * was the one that did not check whose numbers were reported.
 *
 * On a filesystem whose purpose is to survive correctable errors, a
 * checker that trips over one is worse than no checker: it manufactures
 * inconsistencies that are not there, and every hour spent on those is
 * an hour not spent on the real ones.
 *
 * So: nothing reads the device directly any more. These functions
 * return data that has been verified and corrected, or an error saying
 * why they could not. A caller cannot forget to check, because there is
 * nothing to forget -- the check happens on the way in.
 */

#ifndef BEAMFS_FSCK_READ_H
#define BEAMFS_FSCK_READ_H

#include <stdint.h>
#include <stdbool.h>

#include "beamfs_format.h"
#include "rs_decode.h"

/* What happened on the way in. */
enum fsck_read_status {
	/* Read and verified with nothing to correct. */
	FSCK_READ_CLEAN = 0,
	/* Read, an error was found, and Reed-Solomon corrected it. The
	 * data is good; the medium is not, and the caller may want to
	 * say so.
	 */
	FSCK_READ_CORRECTED,
	/* Read, an error was found, and it is beyond correction. The
	 * data is whatever came off the medium and must not be
	 * believed.
	 */
	FSCK_READ_UNCORRECTABLE,
	/* The block has no parity describing it at all: the slot that
	 * should hold it is entirely zero. Not damage -- a block that
	 * was never described, which on a filesystem that writes parity
	 * with every block means it was never written either.
	 *
	 * Worth its own answer because "beyond correction" says the
	 * medium lost something, and this says the filesystem did.
	 * generic/464 produced one: block 41641 of inode 24, named by
	 * the inode on disk, filled with 0xcd, and its parity slot at
	 * region 2482 + 3584 empty. Reported as beyond correction, it
	 * reads as a medium that ate a block. It is not.
	 */
	FSCK_READ_UNDESCRIBED,
	/* The read itself failed: short read, seek error, bad offset. */
	FSCK_READ_IO,
};

/*
 * Reader state.
 *
 * The codec is allocated once rather than per pass. rs_init was called
 * five times in the old code, once in each pass, which is five
 * allocations of the same tables and five places to forget rs_free --
 * pass 4 did forget it on one error path.
 */
struct fsck_reader {
	int              fd;
	struct rs_codec *rs;
	/* Geometry, read once from the superblock and then trusted:
	 * every pass recomputed it and pass 2 and pass 4 disagreed about
	 * the bitmap block count for one release.
	 */
	uint64_t         data_start;
	uint64_t         nblocks;
	uint64_t         inode_table_blk;
	uint64_t         inode_count;
	uint32_t         inodes_per_block;
	/* Indirect-block parity region, zero when the volume predates
	 * it -- v4 and earlier have none, and a checker that insists on
	 * it cannot read them.
	 */
	uint64_t         ind_parity_blk;
	uint32_t         ind_parity_len;
	uint32_t         ind_parity_mode;
	/* Running totals, so a report can say how much of what it found
	 * came off a medium that needed correcting.
	 */
	unsigned int     corrected;
	unsigned int     uncorrectable;
	/* Blocks whose parity slot was empty: never described, so on
	 * this filesystem never written.
	 */
	unsigned int     undescribed;
};

/*
 * Open a device and learn its geometry.
 *
 * The superblock is itself verified and corrected on the way in: a
 * checker that reads its geometry from an uncorrected superblock
 * misreads everything after it, and that failure is silent.
 */
enum fsck_read_status fsck_reader_open(struct fsck_reader *r, int fd);

void fsck_reader_close(struct fsck_reader *r);

/*
 * One inode, verified.
 *
 * Returns CLEAN or CORRECTED with @out holding an inode worth
 * believing; UNCORRECTABLE with @out holding what the medium said,
 * which the caller must not walk.
 *
 * A free slot (i_mode == 0) returns CLEAN without validation, matching
 * what e2fsck does: an unused inode has no CRC worth checking and
 * treating one as damaged would report every empty table as corrupt.
 */
enum fsck_read_status fsck_read_inode(struct fsck_reader *r, uint64_t ino,
				      struct beamfs_inode *out);

/*
 * One indirect block's pointers, verified against the parity region.
 *
 * The old walk_indirect_tree read these with a bare read() and no check
 * of any kind, then followed every pointer it found. A single flipped
 * bit in an indirect block therefore sent the walk somewhere else
 * entirely and orphaned the whole subtree beneath it -- reported as
 * hundreds of lost blocks with nothing wrong with any of them.
 *
 * @out receives BEAMFS_INDIRECT_PTRS little-endian pointers, already
 * byte-swapped to host order so no caller can forget.
 */
enum fsck_read_status fsck_read_indirect(struct fsck_reader *r, uint64_t blk,
					 uint64_t *out);

/*
 * One bitmap block, verified.
 *
 * @out receives BEAMFS_BITMAP_DATA_BYTES of bitmap, with the per-
 * subblock parity stripped and applied. Callers deal in bits, not in
 * the on-disk layout, which is how pass 2 and pass 4 came to walk it
 * differently.
 */
enum fsck_read_status fsck_read_bitmap(struct fsck_reader *r, uint32_t index,
				       uint8_t *out);

/*
 * One data block's payload, flat and verified.
 *
 * A block on disk is sixteen subblocks of data each followed by its
 * parity; the payload the filesystem stores is those sixteen data
 * parts laid end to end. Directory records are written into that flat
 * view, so a walk that reads the block as it sits on disk finds a
 * parity slot in the middle of a filename.
 *
 * @out receives BEAMFS_DATA_INLINE_BYTES with the parity applied and
 * removed.
 */
enum fsck_read_status fsck_read_data(struct fsck_reader *r, uint64_t blk,
				     uint8_t *out);

/* Human-readable, for a message that names what went wrong. */
const char *fsck_read_strerror(enum fsck_read_status s);

#endif /* BEAMFS_FSCK_READ_H */
