// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs -- parity for indirection blocks
 *
 * Author: Aurelien Desbrieres <aurelien@hackers.camp>
 *
 * An indirect block is 512 raw __le64 pointers filling all 4096 bytes,
 * with nowhere to put a checksum. beamfs_check_intermediate_block()
 * closes the dominant failure mode -- a pointer outside the data range,
 * or onto a block the bitmap says is free -- and leaves one open: a
 * flipped pointer that lands in range, on an allocated block, passes
 * every check there is. It is then indistinguishable from a valid
 * pointer, and the read returns someone else's data.
 *
 * The asymmetry is what makes this worth closing. A data block gets
 * eight correctable symbols per 255-byte subblock. A double-indirect
 * pointer gets none, and losing it costs 262144 blocks.
 *
 * Parity lives in a region of its own rather than inside the block, so
 * BEAMFS_INDIRECT_PTRS stays at 512 and no BEAMFS_MAX_IBLOCK_* moves --
 * the format churn that data-protection-design.md section 6.1 argues
 * against does not happen.
 *
 * The slot for a block is computed, not looked up: parity for physical
 * block P sits at (P - s_data_start) * bytes_per_block into the region.
 * A table would be metadata needing protection itself, and the
 * recursion has to stop somewhere.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include "beamfs.h"

/* Parity bytes one indirect block costs under the active mode. */
static size_t ind_parity_stride(struct beamfs_sb_info *sbi)
{
	switch (sbi->s_ind_parity_mode) {
	case BEAMFS_IND_PARITY_CRC:
		return BEAMFS_IND_PARITY_CRC_BYTES;
	case BEAMFS_IND_PARITY_RS:
		return BEAMFS_IND_PARITY_RS_BYTES;
	default:
		return 0;
	}
}

/* Slots one region block holds under the active mode. */
static unsigned int ind_parity_slots(struct beamfs_sb_info *sbi)
{
	switch (sbi->s_ind_parity_mode) {
	case BEAMFS_IND_PARITY_CRC:
		return BEAMFS_IND_PARITY_CRC_SLOTS;
	case BEAMFS_IND_PARITY_RS:
		return BEAMFS_IND_PARITY_RS_SLOTS;
	default:
		return 0;
	}
}

/*
 * Locate the parity for @phys: which region block holds it, and at what
 * offset into that block's payload. Returns false when the mode is off
 * or the block sits outside the data area, both of which mean there is
 * nothing to do.
 *
 * The offset is into the decoded payload, not the raw block. A region
 * block is RS-encoded like a data block, so its 4096 bytes hold 3824 of
 * payload interleaved with parity, and indexing the raw bytes would
 * land in the middle of a codeword.
 */
static bool ind_parity_slot(struct super_block *sb, u64 phys,
			    u64 *region_blk, u32 *offset, size_t *stride)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned int slots;
	u64 index;
	u32 within;

	*stride = ind_parity_stride(sbi);
	slots = ind_parity_slots(sbi);
	if (!*stride || !slots || !sbi->s_ind_parity_blk)
		return false;
	if (phys < sbi->s_data_start)
		return false;

	index = phys - sbi->s_data_start;
	within = (u32)do_div(index, slots);   /* index becomes the quotient */
	*region_blk = sbi->s_ind_parity_blk + index;
	*offset = within * (u32)*stride;

	if (*offset + *stride > BEAMFS_DATA_INLINE_BYTES)
		return false;
	if (*region_blk >= sbi->s_ind_parity_blk + sbi->s_ind_parity_len)
		return false;

	return true;
}

/*
 * Decode a region block into @scratch, correcting what RS can.
 *
 * Returns 0 when the payload can be trusted, -EUCLEAN when a subblock
 * was beyond correction. On -EUCLEAN the caller must not write the
 * block back: re-encoding damaged payload makes the damage permanent
 * and self-consistent, which is worse than leaving it visible.
 *
 * Decoding happens in scratch, never in the buffer: decode_rs8 corrects
 * in place and would rewrite the region itself, which is the mistake
 * beamfs_ind_parity_verify documents below at the cost of 500-block
 * leaks.
 */
static int ind_region_read(struct super_block *sb, struct buffer_head *pbh,
			   u8 *scratch)
{
	int results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	int positions[BEAMFS_DATA_INLINE_SUBBLOCKS * (BEAMFS_RS_PARITY / 2)];
	unsigned int i;
	int ret = 0;

	memcpy(scratch, pbh->b_data, BEAMFS_BLOCK_SIZE);

	beamfs_rs_decode_region(scratch, BEAMFS_SUBBLOCK_TOTAL,
				scratch + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS,
				results, positions,
				BEAMFS_RS_PARITY / 2,
				"indirect parity region");

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		if (results[i] < 0) {
			pr_err_ratelimited("beamfs: parity region block %llu subblock %u beyond correction\n",
					   (unsigned long long)pbh->b_blocknr, i);
			ret = -EUCLEAN;
		} else if (results[i] > 0) {
			beamfs_log_rs_event_flagged(sb, pbh->b_blocknr,
				positions + (size_t)i * (BEAMFS_RS_PARITY / 2),
				(unsigned int)results[i],
				BEAMFS_SUBBLOCK_DATA,
				beamfs_rs_event_subblock_bits(i));
		}
	}
	return ret;
}

/*
 * Copy one slot out of a decoded region block.
 *
 * The payload is interleaved with parity -- 239 bytes of data every 255
 * -- so a slot at payload offset @off can straddle the boundary between
 * two subblocks, and a caller cannot simply point into the buffer.
 *
 * Gathering the whole 3824-byte payload into a second scratch page was
 * the first way this worked, and it cost a page per verify on a read
 * path. beamfs_ind_parity_verify then held three at once out of a pool
 * of nine; generic/464 emptied it and three readers stalled 191 seconds
 * each in __bread_gfp, on a machine with 16 MiB free and 7.6 GiB of
 * page cache that GFP_NOFS could not reclaim. A slot is 256 bytes and
 * fits on the stack.
 */
static void ind_slot_gather(const u8 *scratch, u32 off, size_t stride,
			    u8 *out)
{
	size_t done = 0;

	while (done < stride) {
		unsigned int sub = (unsigned int)((off + done) / BEAMFS_SUBBLOCK_DATA);
		size_t within = (off + done) % BEAMFS_SUBBLOCK_DATA;
		size_t run = BEAMFS_SUBBLOCK_DATA - within;

		if (run > stride - done)
			run = stride - done;

		memcpy(out + done,
		       scratch + (size_t)sub * BEAMFS_SUBBLOCK_TOTAL + within,
		       run);
		done += run;
	}
}

/* Put one slot back into a decoded region block, same geometry. */
static void ind_slot_scatter(u8 *scratch, u32 off, size_t stride,
			     const u8 *in)
{
	size_t done = 0;

	while (done < stride) {
		unsigned int sub = (unsigned int)((off + done) / BEAMFS_SUBBLOCK_DATA);
		size_t within = (off + done) % BEAMFS_SUBBLOCK_DATA;
		size_t run = BEAMFS_SUBBLOCK_DATA - within;

		if (run > stride - done)
			run = stride - done;

		memcpy(scratch + (size_t)sub * BEAMFS_SUBBLOCK_TOTAL + within,
		       in + done, run);
		done += run;
	}
}

/* Encode @scratch, the decoded block, back into the buffer. */
static void ind_region_write(struct buffer_head *pbh, const u8 *scratch)
{
	unsigned int i;

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy((u8 *)pbh->b_data + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       scratch + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);

	beamfs_rs_encode_region((u8 *)pbh->b_data, BEAMFS_SUBBLOCK_TOTAL,
				(u8 *)pbh->b_data + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS);
}

/*
 * beamfs_ind_parity_update -- recompute parity for an indirect block.
 *
 * Takes the buffer_head rather than a block number: b_blocknr already
 * holds the physical block, so no call site has to name the right
 * variable among indirect_blk, dindirect_blk, l1_blk and the rest, and
 * none can pass the wrong one.
 *
 * Call after modifying the block and before releasing it. A missed call
 * leaves stale parity, which reads as corruption on the next verify --
 * worse than no parity at all, since it turns a healthy volume into one
 * that reports damage.
 *
 * @inode is the inode whose write dirtied the block, or NULL when there
 * is none -- the scrubber repairing a block nobody is writing. It is
 * needed because the parity block has to go on that inode's metadata
 * list: a buffer marked dirty and on no inode's list is never flushed
 * by __writeback_single_inode, so the parity stays in memory while the
 * block it describes reaches the disk, and the next verify reads the
 * mismatch as damage.
 *
 * generic/476 left 293 indirect blocks the checker could not decode,
 * 287 of them almost entirely zero: freshly allocated, a few pointers
 * installed, and parity that never followed them down.
 */
void beamfs_ind_parity_update(struct super_block *sb, struct buffer_head *bh,
			      struct inode *inode)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	const void *block = bh->b_data;
	u64 phys = bh->b_blocknr;
	struct buffer_head *pbh;
	u64 region_blk;
	u32 offset;
	size_t stride;
	unsigned int i;
	u8 *scratch;
	/* One slot, 256 bytes. On the stack, so this path holds one
	 * scratch page instead of two.
	 */
	u8 slotbuf[BEAMFS_IND_PARITY_RS_BYTES];

	if (!ind_parity_slot(sb, phys, &region_blk, &offset, &stride))
		return;
	if (stride > sizeof(slotbuf))
		return;

	pbh = beamfs_bread(sb, region_blk, "indirect parity");
	if (!pbh) {
		pr_err_ratelimited("beamfs: cannot read parity block %llu for indirect %llu\n",
				   (unsigned long long)region_blk,
				   (unsigned long long)phys);
		return;
	}

	scratch = beamfs_scratch_get(sb);
	if (!scratch) {
		brelse(pbh);
		return;
	}

	lock_buffer(pbh);

	/*
	 * Read-modify-write on the payload. The region block carries its
	 * own FEC, so one slot cannot be edited in the raw bytes: the
	 * whole block has to be decoded, the slot replaced, and the block
	 * re-encoded.
	 *
	 * A region block beyond correction is left alone. Re-encoding it
	 * would write parity over damaged payload and make fifteen other
	 * indirect blocks' parity permanently wrong, in a way nothing
	 * could afterwards detect.
	 */
	if (ind_region_read(sb, pbh, scratch) == -EUCLEAN) {
		pr_err_ratelimited("beamfs: parity region block %llu beyond correction; not updating the slot for indirect %llu\n",
				   (unsigned long long)region_blk,
				   (unsigned long long)phys);
		unlock_buffer(pbh);
		beamfs_scratch_put(sb, scratch);
		brelse(pbh);
		return;
	}

	ind_slot_gather(scratch, offset, stride, slotbuf);

	if (sbi->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		__le32 *slot = (__le32 *)slotbuf;

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			slot[i] = cpu_to_le32(crc32_le(~0U,
				(const u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA) ^ ~0U);
	} else {
		u8 *slot = slotbuf;

		/*
		 * The indirect block is 4096 bytes and an RS codeword
		 * covers 239, so it is split the way a data block is: 16
		 * subblocks, one 16-byte parity set each. The last
		 * subblock runs past 3824 into the tail, deliberately --
		 * the tail is part of the pointer array and needs
		 * covering too.
		 */
		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			beamfs_rs_encode_region(
				(u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA,
				slot + (size_t)i * BEAMFS_RS_PARITY,
				BEAMFS_RS_PARITY,
				BEAMFS_SUBBLOCK_DATA, 1);
	}

	ind_slot_scatter(scratch, offset, stride, slotbuf);
	ind_region_write(pbh, scratch);
	unlock_buffer(pbh);
	beamfs_scratch_put(sb, scratch);
	/*
	 * On the inode's list when there is one, so writeback carries it
	 * with the block it describes. Without an inode -- the scrubber
	 * -- a plain dirty is all there is, and sync_blockdev is what
	 * eventually takes it.
	 */
	if (inode)
		mmb_mark_buffer_dirty(pbh, &BEAMFS_I(inode)->i_metadata_bhs);
	else
		mark_buffer_dirty(pbh);
	brelse(pbh);
}

/*
 * beamfs_ind_parity_verify -- check an indirect block against its parity.
 *
 * Under CRC the block is checked and left alone: detection turns the
 * section 6.1 residual from silent corruption into a clean fail-closed
 * error, which is the contract beamfs states everywhere else. Under RS
 * the block is corrected in place, so a hit pointer is repaired and the
 * file survives.
 *
 * Returns 0 if the block is sound or was corrected, -EUCLEAN if it is
 * damaged beyond the mode's ability to fix.
 */
int beamfs_ind_parity_verify(struct super_block *sb, struct buffer_head *bh)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	void *block = bh->b_data;
	u64 phys = bh->b_blocknr;
	struct buffer_head *pbh;
	u64 region_blk;
	u32 offset;
	size_t stride;
	unsigned int i;
	int ret = 0;
	u8 *rscratch;
	u8 slotbuf[BEAMFS_IND_PARITY_RS_BYTES];

	if (!ind_parity_slot(sb, phys, &region_blk, &offset, &stride))
		return 0;

	pbh = beamfs_bread(sb, region_blk, "indirect parity");
	if (!pbh)
		return 0;   /* parity unreadable: do not fail the read on it */

	if (stride > sizeof(slotbuf)) {
		brelse(pbh);
		return 0;
	}
	rscratch = beamfs_scratch_get(sb);
	if (!rscratch) {
		brelse(pbh);
		return 0;
	}

	/*
	 * The region block carries its own FEC, so the slot has to be
	 * decoded out of it rather than read from the raw bytes.
	 *
	 * A region beyond correction is not the indirect block's fault
	 * and must not be reported as such: before this, a hit on the
	 * region made every indirect block it covered look damaged, and
	 * fsck blamed the blocks. Saying so and passing the read is the
	 * honest answer -- the block may well be fine and there is now
	 * nothing to check it against.
	 */
	if (ind_region_read(sb, pbh, rscratch) == -EUCLEAN) {
		pr_err_ratelimited("beamfs: parity region block %llu beyond correction; indirect %llu cannot be checked\n",
				   (unsigned long long)region_blk,
				   (unsigned long long)phys);
		beamfs_scratch_put(sb, rscratch);
		brelse(pbh);
		return 0;
	}

	ind_slot_gather(rscratch, offset, stride, slotbuf);

	if (sbi->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		const __le32 *slot = (const __le32 *)slotbuf;

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			u32 want = le32_to_cpu(slot[i]);
			u32 got = crc32_le(~0U,
				(const u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA) ^ ~0U;

			if (want != got) {
				pr_err_ratelimited("beamfs: indirect block %llu subblock %u CRC mismatch\n",
						   (unsigned long long)phys, i);
				beamfs_log_rs_event_flagged(sb,
					phys, NULL, 0, BEAMFS_SUBBLOCK_DATA,
					beamfs_rs_event_subblock_bits(i));
				ret = -EUCLEAN;
			}
		}
	} else {
		u8 *slot = slotbuf;
		int results[1];
		int positions[BEAMFS_RS_PARITY / 2];
		u8 *copy;

		/*
		 * Decode a copy, never bh->b_data.
		 *
		 * decode_rs8 corrects in place: it writes both the data
		 * and the parity it is handed. Decoding the live indirect
		 * block against the parity region rewrites that block --
		 * and when the region's parity is stale, which it is the
		 * moment a data block is reallocated as an indirect one
		 * without the region being updated, the "correction"
		 * drags the block back toward whatever it held before.
		 *
		 * generic/464 turned that into 500-block leaks: a fresh
		 * L1 filled with 512 pointers was read once by another
		 * process, verify decoded it against block 1934's old
		 * parity, and the 512 pointers collapsed back to the two
		 * the previous owner had left -- orphaning everything
		 * below them.
		 *
		 * A verify has no business modifying the block it checks.
		 * It decodes into scratch, reports what it finds, and
		 * touches nothing. The only real corruption on a device
		 * that flips no bits is stale parity, and the fix for that
		 * is to keep the parity current, not to let the decoder
		 * launder one block's contents into another's.
		 */
		copy = beamfs_scratch_get(sb);
		if (!copy) {
			beamfs_scratch_put(sb, rscratch);
			brelse(pbh);
			return 0;
		}
		memcpy(copy, block, BEAMFS_BLOCK_SIZE);

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			int rc = beamfs_rs_decode_region(
				copy + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA,
				slot + (size_t)i * BEAMFS_RS_PARITY,
				BEAMFS_RS_PARITY,
				BEAMFS_SUBBLOCK_DATA, 1,
				results, positions, BEAMFS_RS_PARITY / 2,
				"indirect");

			if (rc < 0 || results[0] < 0) {
				pr_err_ratelimited("beamfs: indirect block %llu subblock %u uncorrectable\n",
						   (unsigned long long)phys, i);
				beamfs_log_rs_event_flagged(sb,
					phys, NULL, 0, BEAMFS_SUBBLOCK_DATA,
					beamfs_rs_event_subblock_bits(i));
				ret = -EUCLEAN;
			} else if (results[0] > 0) {
				beamfs_log_rs_event_flagged(sb,
					phys, positions, (unsigned int)results[0],
					BEAMFS_SUBBLOCK_DATA,
					beamfs_rs_event_subblock_bits(i));
			}
		}
		beamfs_scratch_put(sb, copy);
	}

	beamfs_scratch_put(sb, rscratch);
	brelse(pbh);
	return ret;
}
