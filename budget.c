// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs -- per-block error budget
 *
 * Author: Aurelien Desbrieres <aurelien@hackers.camp>
 *
 * How much correction capacity a block has left, rather than how many
 * corrections it has had.
 *
 * The RS journal holds the last 64 events. That answers "what happened
 * recently" and says nothing about the state of the volume: a block
 * that has quietly taken seven of its eight correctable symbols reads
 * back perfectly and is one upset away from unrecoverable, and nothing
 * anywhere would say so before the read that fails.
 *
 * One byte per data block records the worst subblock's corrected-symbol
 * count. An operator can then ask how much margin is left -- a fuel
 * gauge rather than an odometer.
 *
 * btrfs and ZFS count corrections and do not report margin, because
 * with redundancy elsewhere there is no margin to report: a bad copy is
 * replaced from a good one and the question does not arise. On a single
 * device it does, and the answer is finite and worth knowing.
 *
 * The byte is a high-water mark, not a running total. What matters is
 * the worst any subblock of the block has been, since that is the one
 * that will saturate first; a count that decayed would report a
 * healthier block than the medium justifies.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include "beamfs.h"

/* Is the feature active on this volume? */
static bool budget_active(struct beamfs_sb_info *sbi)
{
	return sbi->s_budget_blk != 0 && sbi->s_budget_len != 0;
}

/*
 * Where the byte for @phys lives: which region block, and the offset
 * within it. Computed, not looked up -- a table mapping blocks to
 * slots would be metadata needing protection of its own, and the
 * recursion has to stop somewhere.
 */
static bool budget_slot(struct super_block *sb, u64 phys,
			u64 *region_blk, u32 *offset)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u64 index;

	if (!budget_active(sbi) || phys < sbi->s_data_start)
		return false;

	index = phys - sbi->s_data_start;
	*region_blk = sbi->s_budget_blk + index / BEAMFS_BLOCK_SIZE;
	*offset = (u32)(index % BEAMFS_BLOCK_SIZE);

	return *region_blk < sbi->s_budget_blk + sbi->s_budget_len;
}

/*
 * beamfs_budget_record -- note that @phys needed @symbols corrections.
 *
 * Raises the high-water mark if this is the worst the block has seen.
 * A no-op when the value would not change, so a volume being read
 * repeatedly does not rewrite the region for nothing.
 */
void beamfs_budget_record(struct super_block *sb, u64 phys,
			  unsigned int symbols)
{
	struct buffer_head *bh;
	u64 region_blk;
	u32 offset;
	u8 capped;

	if (!symbols || !budget_slot(sb, phys, &region_blk, &offset))
		return;

	capped = (u8)min_t(unsigned int, symbols, BEAMFS_ERROR_BUDGET_MAX);

	bh = sb_bread(sb, region_blk);
	if (!bh)
		return;

	if (((u8 *)bh->b_data)[offset] < capped) {
		lock_buffer(bh);
		((u8 *)bh->b_data)[offset] = capped;
		unlock_buffer(bh);
		mark_buffer_dirty(bh);
	}
	brelse(bh);
}

/*
 * beamfs_budget_read -- worst corrected-symbol count seen for @phys.
 *
 * Returns 0 when the feature is off, which reads as "no wear known"
 * rather than "no wear" -- the distinction belongs to the caller.
 */
u8 beamfs_budget_read(struct super_block *sb, u64 phys)
{
	struct buffer_head *bh;
	u64 region_blk;
	u32 offset;
	u8 v;

	if (!budget_slot(sb, phys, &region_blk, &offset))
		return 0;

	bh = sb_bread(sb, region_blk);
	if (!bh)
		return 0;
	v = ((u8 *)bh->b_data)[offset];
	brelse(bh);

	return v;
}

/*
 * beamfs_budget_histogram -- how many blocks sit at each wear level.
 *
 * @hist must hold BEAMFS_ERROR_BUDGET_MAX + 1 entries. Index n is the
 * number of blocks whose worst subblock needed n corrections, so
 * hist[0] is untouched blocks and hist[MAX] is blocks with no margin
 * left.
 *
 * The whole region is read, which on a 931 GiB volume is 244 MiB. That
 * is a deliberate cost paid on request: the alternative is maintaining
 * counters in memory, which would have to be reconstructed at mount
 * anyway and would drift against the medium in between.
 */
void beamfs_budget_histogram(struct super_block *sb, u64 *hist)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u64 blk;
	u64 seen = 0;

	memset(hist, 0, (BEAMFS_ERROR_BUDGET_MAX + 1) * sizeof(*hist));
	if (!budget_active(sbi))
		return;

	for (blk = 0; blk < sbi->s_budget_len; blk++) {
		struct buffer_head *bh = sb_bread(sb, sbi->s_budget_blk + blk);
		u32 i;

		if (!bh)
			continue;

		for (i = 0; i < BEAMFS_BLOCK_SIZE; i++) {
			u8 v = ((u8 *)bh->b_data)[i];

			if (seen >= sbi->s_nblocks)
				break;
			seen++;
			if (v > BEAMFS_ERROR_BUDGET_MAX)
				v = BEAMFS_ERROR_BUDGET_MAX;
			hist[v]++;
		}
		brelse(bh);
		cond_resched();
	}
}
