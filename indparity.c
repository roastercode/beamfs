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

/*
 * Locate the parity for @phys: which region block holds it, and at what
 * offset. Returns false when the mode is off or the block sits outside
 * the data area, both of which mean there is nothing to do.
 */
static bool ind_parity_slot(struct super_block *sb, u64 phys,
			    u64 *region_blk, u32 *offset, size_t *stride)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u64 index, byte_off;

	*stride = ind_parity_stride(sbi);
	if (!*stride || !sbi->s_ind_parity_blk)
		return false;
	if (phys < sbi->s_data_start)
		return false;

	index = phys - sbi->s_data_start;
	byte_off = index * *stride;

	*region_blk = sbi->s_ind_parity_blk + byte_off / BEAMFS_BLOCK_SIZE;
	*offset = (u32)(byte_off % BEAMFS_BLOCK_SIZE);

	/*
	 * A slot straddling two region blocks would need two reads and a
	 * split write. Both strides divide 4096 evenly (64 and 256), so
	 * this cannot happen; the check is here because a future mode
	 * with an awkward stride would otherwise corrupt silently.
	 */
	if (*offset + *stride > BEAMFS_BLOCK_SIZE)
		return false;
	if (*region_blk >= sbi->s_ind_parity_blk + sbi->s_ind_parity_len)
		return false;

	return true;
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
 */
void beamfs_ind_parity_update(struct super_block *sb, struct buffer_head *bh)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	const void *block = bh->b_data;
	u64 phys = bh->b_blocknr;
	struct buffer_head *pbh;
	u64 region_blk;
	u32 offset;
	size_t stride;
	unsigned int i;

	if (!ind_parity_slot(sb, phys, &region_blk, &offset, &stride))
		return;

	pbh = beamfs_bread(sb, region_blk, "indirect parity");
	if (!pbh) {
		pr_err_ratelimited("beamfs: cannot read parity block %llu for indirect %llu\n",
				   (unsigned long long)region_blk,
				   (unsigned long long)phys);
		return;
	}

	lock_buffer(pbh);
	if (sbi->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		__le32 *slot = (__le32 *)((u8 *)pbh->b_data + offset);

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			slot[i] = cpu_to_le32(crc32_le(~0U,
				(const u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA) ^ ~0U);
	} else {
		u8 *slot = (u8 *)pbh->b_data + offset;

		/*
		 * The block is 4096 bytes and an RS codeword covers 239, so
		 * it is split the same way a data block is: 16 subblocks,
		 * one 16-byte parity set each. The last subblock runs past
		 * 3824 into the tail; that is deliberate, the tail is part
		 * of the pointer array and needs covering too.
		 */
		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			beamfs_rs_encode_region(
				(u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA,
				slot + (size_t)i * BEAMFS_RS_PARITY,
				BEAMFS_RS_PARITY,
				BEAMFS_SUBBLOCK_DATA, 1);
	}
	unlock_buffer(pbh);
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

	if (!ind_parity_slot(sb, phys, &region_blk, &offset, &stride))
		return 0;

	pbh = beamfs_bread(sb, region_blk, "indirect parity");
	if (!pbh)
		return 0;   /* parity unreadable: do not fail the read on it */

	if (sbi->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		const __le32 *slot = (const __le32 *)((u8 *)pbh->b_data + offset);

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			u32 want = le32_to_cpu(slot[i]);
			u32 got = crc32_le(~0U,
				(const u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_DATA) ^ ~0U;

			if (want != got) {
				pr_err_ratelimited("beamfs: indirect block %llu subblock %u CRC mismatch\n",
						   (unsigned long long)phys, i);
				beamfs_log_rs_event_flagged(sb,
					phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
					NULL, 0, BEAMFS_SUBBLOCK_DATA, 0);
				ret = -EUCLEAN;
			}
		}
	} else {
		u8 *slot = (u8 *)pbh->b_data + offset;
		int results[1];
		int positions[BEAMFS_RS_PARITY / 2];

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
			int rc = beamfs_rs_decode_region(
				(u8 *)block + (size_t)i * BEAMFS_SUBBLOCK_DATA,
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
					phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
					NULL, 0, BEAMFS_SUBBLOCK_DATA, 0);
				ret = -EUCLEAN;
			} else if (results[0] > 0) {
				beamfs_log_rs_event(sb,
					phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
					positions, (unsigned int)results[0],
					BEAMFS_SUBBLOCK_DATA);
			}
		}
	}

	brelse(pbh);
	return ret;
}
