// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs -- background scrubber
 *
 * Author: Aurelien Desbrieres <aurelien@hackers.camp>
 *
 * beamfs corrects on read. A block nobody reads therefore accumulates
 * upsets until it passes the eight-symbol correction radius and becomes
 * unrecoverable, with nothing having reported the drift on the way.
 * Cold data is the case this filesystem exists for -- an archive
 * written once and read years later -- so waiting for a reader is
 * waiting for the failure.
 *
 * The scrubber walks allocated blocks at a bounded rate and decodes
 * each one. It does not write back. Rewriting cold data turns a read
 * into a read-modify-write with a power-loss window, and the corrected
 * bytes the decode produced are what a subsequent reader would get
 * anyway. Detection is the point: an operator who learns a volume is
 * drifting can act while the drift is still correctable, which is the
 * whole difference between a warning and a post-mortem.
 *
 * Rate rather than priority. The threat-model text called for RT
 * priority; a thread that reads every block on the device at RT
 * priority would starve the workload it is meant to protect. The
 * bounded interval gives the same guarantee that matters -- a full
 * sweep completes in a knowable time -- without that cost. The
 * interval is per-block and settable through sysfs, so an operator
 * sizes the sweep against the expected upset rate.
 */

#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include "beamfs.h"

#define BEAMFS_SCRUB_DEFAULT_INTERVAL_MS  100

/*
 * The pace a volume starts at, in milliseconds between blocks.
 *
 * Per-volume it is settable through sysfs once mounted, which is no
 * help for a volume something else mounts: xfstests makes and mounts
 * its scratch device itself, and the scrubber is running on it before
 * anything outside the kernel can say otherwise. Worse, the root
 * filesystem here is beamfs too, so a sweep was running under every
 * measurement ever taken on this machine -- ten blocks a second, each
 * one seventeen Reed-Solomon decodes, halving its interval again on
 * every correction.
 *
 * As a parameter it is settable before the mount rather than after:
 * beamfs.scrub_interval_ms= on the kernel command line, or through
 * /sys/module/beamfs/parameters/ for the mounts still to come. 0 parks
 * the scrubber, which is a measurement decision and not a safe
 * default: correction on read is unaffected, but nothing then goes
 * looking for the block nobody reads.
 */
static unsigned int scrub_interval_ms = BEAMFS_SCRUB_DEFAULT_INTERVAL_MS;
module_param(scrub_interval_ms, uint, 0644);
MODULE_PARM_DESC(scrub_interval_ms,
		 "milliseconds between blocks for newly mounted volumes (0 parks the scrubber)");

/*
 * beamfs_scrub_check_block -- decode one block, report what it found.
 *
 * @corrected receives the number of subblocks that needed correction.
 * Returns 0 if the block decodes, -EUCLEAN if any subblock exceeded the
 * correction radius, or a negative errno on read failure.
 *
 * Works on the physical block without reference to an inode: the
 * scrubber sweeps the allocation bitmap and does not know, or need to
 * know, which file a block belongs to.
 */
int beamfs_scrub_check_block(struct super_block *sb, u64 phys,
			     unsigned int *corrected)
{
	struct buffer_head *bh;
	/*
	 * On the stack, as in beamfs_dirent_decode: 64 and 512 bytes,
	 * against a 2 KiB frame budget, on a sweep that must not wait on
	 * the allocator while the machine is under pressure.
	 */
	int  results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	int  positions[BEAMFS_DATA_INLINE_SUBBLOCKS * (BEAMFS_RS_PARITY / 2)];
	u8   *staging;
	unsigned int i, n_corrected = 0;
	int ret = 0;
	/*
	 * Which layout the first decode settled on, so the confirming
	 * read takes the same one.
	 */
	bool woven = false;

	*corrected = 0;

	memset(results, 0, sizeof(results));
	staging = beamfs_scratch_get(sb);
	if (!staging) {
		ret = -ENOMEM;
		goto out_free;
	}

	bh = sb_bread(sb, phys);
	if (!bh) {
		ret = -EIO;
		goto out_free;
	}

	/*
	 * Decode into staging rather than in place: the buffer_head is
	 * shared through the block device page cache, and a scrub has no
	 * business modifying what a concurrent reader sees.
	 */
	/*
	 * Under the buffer lock, or the snapshot is of two blocks.
	 *
	 * Writers hold lock_buffer across memcpy plus rs_encode_region --
	 * the payload changes, then the sixteen parity fields catch up.
	 * Copying without the lock catches that halfway and hands the
	 * decoder a payload from after the write with parity from before,
	 * which is uncorrectable by construction.
	 *
	 * That is what the sweep kept reporting: block 1110 failing all
	 * sixteen codewords while fsck called the same volume clean, and
	 * blocks 1062 and 1631 failing only the subblocks that held data.
	 * Twenty such reports appeared the moment new data blocks started
	 * being encoded on allocation, because there were simply more
	 * writes to collide with.
	 *
	 * The lock is held for a 3824-byte copy and nothing else. The
	 * decode runs afterwards, on the copy.
	 */
	lock_buffer(bh);
	/*
	 * The whole block, parity included.
	 *
	 * decode_rs8 corrects data and parity in place -- it writes to
	 * both buffers it is given. Copying only the payload and pointing
	 * the decoder at bh->b_data for the parity means every read
	 * rewrites the shared buffer's parity bytes. A block read often
	 * enough degrades until it will not decode at all, which is what
	 * the sweep kept reporting on blocks fsck called clean, and why
	 * the count grew as the volume got busier.
	 *
	 * The data path has done this correctly all along: "Decode
	 * RS(255,239) subblocks into a private scratch buffer, never into
	 * bh->b_data."
	 */
	memcpy(staging, (u8 *)bh->b_data, BEAMFS_BLOCK_SIZE);
	unlock_buffer(bh);

	/*
	 * Which layout, when the sweep does not know what it is holding.
	 *
	 * Two of the three callers know: the children of a level-one
	 * indirect block and an inode's i_direct are data blocks, and on
	 * an interleaved volume they are capsules. The third walks the
	 * wear map and visits whatever is allocated -- a capsule, an
	 * indirect block, a directory block -- with nothing to say
	 * which.
	 *
	 * So: try the layout the volume uses for data, and fall back to
	 * the alternating one. A block does not decode cleanly under
	 * both by accident -- sixteen codewords of sixteen parity bytes
	 * each agreeing on the wrong symbols is not something that
	 * happens -- so whichever succeeds is the one it was written in.
	 *
	 * Without this the sweep reported "block 17725 subblock 6/16
	 * uncorrectable" on a block that was intact, which is worse than
	 * missing damage: it condemns what it was meant to protect.
	 */
	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) {
		woven = true;
		ret = beamfs_rs_decode_woven(staging + BEAMFS_CAPSULE_DATA_OFF,
					     staging + BEAMFS_CAPSULE_PARITY_OFF,
					     BEAMFS_RS_PARITY,
					     BEAMFS_SUBBLOCK_DATA,
					     BEAMFS_DATA_INLINE_SUBBLOCKS,
					     results, "sweep");
		if (ret != -EUCLEAN)
			goto decoded;

		/* Not a capsule: an indirect or directory block. */
		woven = false;
		memcpy(staging, (u8 *)bh->b_data, BEAMFS_BLOCK_SIZE);
	}

	ret = beamfs_rs_decode_region(staging, BEAMFS_SUBBLOCK_TOTAL,
				      staging + BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_SUBBLOCK_TOTAL,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS,
				      results, positions,
				      BEAMFS_RS_PARITY / 2,
				"sweep");
decoded:

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		if (results[i] > 0) {
			n_corrected++;
			beamfs_log_rs_event_flagged(sb,
				phys,
				positions + (size_t)i * (BEAMFS_RS_PARITY / 2),
				(unsigned int)results[i],
				BEAMFS_SUBBLOCK_DATA,
				beamfs_rs_event_subblock_bits(i));
			/*
			 * The sweep is where the budget earns its keep: it
			 * visits blocks nobody reads, which are the ones
			 * that drift unobserved.
			 */
			beamfs_budget_record(sb, phys,
					     (unsigned int)results[i]);
		} else if (results[i] < 0) {
			/*
			 * Confirm by re-reading before crying wolf. The
			 * sweep reads a block via sb_bread on the block
			 * device while the owning inode may still be
			 * flushing, and can catch data updated with parity
			 * not yet written, or the reverse -- uncorrectable
			 * on a block that is sound once the flush lands.
			 * Every first-boot report of this kind decoded
			 * clean offline, byte-identical on disk. A real
			 * defect survives a re-read; a half-flushed block
			 * does not.
			 */
			struct buffer_head *rbh;
			int rc2 = -EUCLEAN;

			if (bh) {
				clear_buffer_uptodate(bh);
				brelse(bh);
				bh = NULL;
			}
			rbh = __bread(sb->s_bdev, phys, BEAMFS_BLOCK_SIZE);
			if (rbh) {
				int r2[BEAMFS_DATA_INLINE_SUBBLOCKS];
				int p2[BEAMFS_DATA_INLINE_SUBBLOCKS *
				       (BEAMFS_RS_PARITY / 2)];

				lock_buffer(rbh);
				memcpy(staging, (u8 *)rbh->b_data,
				       BEAMFS_BLOCK_SIZE);
				unlock_buffer(rbh);
				/*
				 * The confirming read takes the layout
				 * the first decode settled on. Reading
				 * it the other way would confirm damage
				 * that is not there, which is the whole
				 * reason this second read exists.
				 */
				if (woven)
					beamfs_rs_decode_woven(
						staging + BEAMFS_CAPSULE_DATA_OFF,
						staging + BEAMFS_CAPSULE_PARITY_OFF,
						BEAMFS_RS_PARITY,
						BEAMFS_SUBBLOCK_DATA,
						BEAMFS_DATA_INLINE_SUBBLOCKS,
						r2, "sweep-confirm");
				else
					beamfs_rs_decode_region(staging,
						BEAMFS_SUBBLOCK_TOTAL,
						staging + BEAMFS_SUBBLOCK_DATA,
						BEAMFS_SUBBLOCK_TOTAL,
						BEAMFS_SUBBLOCK_DATA,
						BEAMFS_DATA_INLINE_SUBBLOCKS,
						r2, p2, BEAMFS_RS_PARITY / 2,
						"sweep-confirm");
				rc2 = r2[i];
				brelse(rbh);
			}
			if (rc2 >= 0)
				continue;
			/*
			 * Say which block and which codeword. An
			 * uncorrectable subblock is the event this
			 * filesystem exists to report, and a line that
			 * names neither is an alarm nobody can follow.
			 */
			pr_err_ratelimited("beamfs: sweep: block %llu subblock %u/%u uncorrectable\n",
					   (unsigned long long)phys, i,
					   BEAMFS_DATA_INLINE_SUBBLOCKS);
			/*
			 * Past the radius. Journalled with the same shape
			 * the read path uses, so a forensic pass cannot tell
			 * whether a reader or the scrubber found it -- which
			 * is correct: the damage is the same either way.
			 */
			beamfs_log_rs_event_flagged(sb,
				phys,
				NULL, 0, BEAMFS_SUBBLOCK_DATA,
				beamfs_rs_event_subblock_bits(i));
			ret = -EUCLEAN;
		}
	}

	/*
	 * Write the repair back, or it was not a repair.
	 *
	 * The decoder puts corrected data in staging and corrected parity
	 * straight into the buffer; without this the buffer was released
	 * and both were discarded. The block on disk stayed damaged, the
	 * next sweep found the same errors, and the counter climbed
	 * forever -- measured: 120 corrections of one block over 119
	 * sweeps, none of which fixed anything.
	 *
	 * That is the failure this filesystem exists to prevent. Each
	 * upset is individually correctable; what kills the data is their
	 * sum. A scrubber that detects without repairing lets them
	 * accumulate to the ninth, and the ninth is unrecoverable.
	 *
	 * Written under the buffer lock so a concurrent reader sees the
	 * block whole, and only when something was corrected: an
	 * untouched block must not be dirtied, or a quiet volume would
	 * rewrite itself endlessly and wear the medium for nothing.
	 */
	/*
	 * bh is NULL when the confirm path above dropped it. A block with
	 * both a corrected and an uncorrectable subblock skips write-back
	 * this pass; it is damaged, and the next sweep retries from a
	 * fresh read.
	 */
	if (n_corrected && bh) {
		lock_buffer(bh);
		/*
		 * The whole block. staging holds the corrected codewords
		 * in their on-disk layout now -- data and parity both, at
		 * a 255-byte stride -- because that is what decode_rs8
		 * produces and what the block has to go back as.
		 */
		memcpy((u8 *)bh->b_data, staging, BEAMFS_BLOCK_SIZE);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty(bh);
	}

	*corrected = n_corrected;
	if (bh)
		brelse(bh);

out_free:
	beamfs_scratch_put(sb, staging);
	return ret;
}

/*
 * Walk one inode's data blocks, decoding each.
 *
 * The bitmap says a block is in use; it does not say what it holds.
 * Sweeping it directly meant handing indirect blocks and directory
 * blocks to the RS decoder, which has no parity to find in them, and
 * journalling every one as uncorrectable -- 24 false entries in the
 * first minutes of the first run, in the same journal that exists to
 * be forensic evidence. A scrubber that cries wolf is worse than none.
 *
 * Following the inode's own pointers is what the read path does, so
 * the scrubber sees exactly the blocks a reader would decode and
 * nothing else. Indirect blocks are traversed but not decoded: they
 * carry raw pointers, which is the residual documented in
 * data-protection-design.md section 6.1.
 *
 * Returns the number of data blocks visited.
 */
static u64 beamfs_scrub_walk_level(struct super_block *sb, u64 blk,
				   unsigned int depth,
				   struct beamfs_sb_info *sbi)
{
	u64 nptrs = BEAMFS_BLOCK_SIZE / sizeof(__le64);
	struct buffer_head *ibh;
	__le64 *ptrs;
	u64 visited = 0;
	u64 j;

	if (!blk || !beamfs_block_is_allocated(sb, blk))
		return 0;

	ibh = sb_bread(sb, blk);
	if (!ibh)
		return 0;

	/*
	 * Copy the pointers and let the buffer go, for the same reason
	 * the inode is snapshotted above: the loop sleeps for the pace
	 * interval after every block, and a pointer into a block-device
	 * buffer does not survive that reliably. Reading ptrs[j] after a
	 * recycle gives whatever now occupies the page, and the sweep
	 * then reads and releases blocks at random -- which is how brelse
	 * came to be called on an already-free buffer.
	 *
	 * One page copied per indirect block visited, against a sweep
	 * that walks at one block per interval.
	 */
	/*
	 * Check the indirect block itself before reading it.
	 *
	 * The sweep walked every data block an inode owns and never
	 * looked at the blocks holding the pointers, which carry their
	 * own parity scheme -- ind_parity, not the sixteen RS codewords
	 * a data block uses. A corrupted pointer array is worse than a
	 * corrupted data block: it loses everything below it.
	 */
	if (beamfs_ind_parity_verify(sb, ibh)) {
		pr_err_ratelimited("beamfs: sweep: indirect block %llu fails its parity\n",
				   (unsigned long long)blk);
		sbi->s_scrub_uncorrectable++;
		brelse(ibh);
		return 0;
	}

	/*
	 * The copy under the buffer lock, for the reason
	 * beamfs_scrub_check_block gives about data blocks and that was
	 * never applied to the blocks holding the pointers.
	 *
	 * Every site in file_inline.c that installs a pointer holds
	 * lock_buffer across the store. Copying without it catches the
	 * block halfway, and the walk below then descends into slots
	 * holding neither the old pointer nor the new one.
	 *
	 * The verify above stays outside the lock on purpose. It reads
	 * the parity region through beamfs_bread and takes a scratch page
	 * from a pool of thirty-two, and holding a buffer lock across
	 * that is the shape super.c documents at the scratch pool: three
	 * tasks stalled 191 seconds in __bread_gfp under
	 * beamfs_ind_parity_verify, the machine at 330% CPU, never
	 * returning. A verify that reads a half-written block reports a
	 * block that is sound as uncorrectable, which is noise in dmesg;
	 * a walk over a half-written copy loses pointers, which is not.
	 * The lock goes where the damage is.
	 */
	lock_buffer(ibh);
	ptrs = kmemdup(ibh->b_data, BEAMFS_BLOCK_SIZE, GFP_NOFS);
	unlock_buffer(ibh);
	brelse(ibh);
	if (!ptrs)
		return 0;

	for (j = 0; j < nptrs && !kthread_should_stop(); j++) {
		u64 child = le64_to_cpu(ptrs[j]);
		unsigned int corrected = 0;
		int ret;

		if (!child)
			continue;

		if (depth > 1) {
			visited += beamfs_scrub_walk_level(sb, child,
							   depth - 1, sbi);
			continue;
		}

		if (!beamfs_block_is_allocated(sb, child))
			continue;

		ret = beamfs_scrub_check_block(sb, child, &corrected);
		if (ret == -EUCLEAN)
			pr_err_ratelimited("beamfs: sweep: block %llu is a leaf under indirect %llu (depth %u, slot %llu)\n",
					   (unsigned long long)child,
					   (unsigned long long)blk, depth,
					   (unsigned long long)j);
		visited++;
		sbi->s_scrub_blocks++;
		if (corrected)
			sbi->s_scrub_corrected++;
		if (ret == -EUCLEAN)
			sbi->s_scrub_uncorrectable++;

		msleep_interruptible(READ_ONCE(sbi->s_scrub_interval_ms));
	}

	kfree(ptrs);
	return visited;
}

static void beamfs_scrub_one_inode(struct super_block *sb, unsigned long ino)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_inode   *raw;
	struct buffer_head    *bh;
	unsigned long inodes_per_block, block, offset;
	unsigned int i;
	umode_t mode;

	/*
	 * Skip an inode somebody is writing.
	 *
	 * The sweep reads the inode table and the data blocks straight
	 * off the block device, in parallel with the writeback that is
	 * updating them. It can see a block with its payload written and
	 * its parity not yet, or the reverse, and report uncorrectable on
	 * something no reader would ever see in that state. Every such
	 * report checked out clean once the writer was done: fsck passed
	 * the image, the offline decoder found nothing, and the bytes on
	 * disk were identical to the ones that had just been rejected.
	 *
	 * Re-reading the block confirms the ones that settle between two
	 * reads, but a file being appended to continuously -- a system log
	 * during a test run -- is never settled, and both reads land in
	 * the same window.
	 *
	 * ilookup returns the inode only if it is already in cache; it
	 * never reads from disk, so it cannot recurse into the filesystem.
	 * An inode nobody has open is not being written and is safe to
	 * sweep. One that is dirty or under writeback is skipped and comes
	 * round again on the next pass, which is what a scrubber should do
	 * with a moving target.
	 */
	{
		struct inode *vi = ilookup(sb, ino);

		if (vi) {
			unsigned long st = inode_state_read_once(vi);
			bool busy = (st & (I_DIRTY_ALL | I_SYNC |
					   I_NEW | I_FREEING | I_WILL_FREE)) ||
				    mapping_writably_mapped(vi->i_mapping);

			iput(vi);
			if (busy)
				return;
		}
	}

	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	block  = le64_to_cpu(sbi->s_beamfs_sb->s_inode_table_blk)
		 + (ino - 1) / inodes_per_block;
	offset = (ino - 1) % inodes_per_block;

	bh = sb_bread(sb, block);
	if (!bh)
		return;
	raw = (struct beamfs_inode *)bh->b_data + offset;
	mode = le16_to_cpu(raw->i_mode);

	/*
	 * Take a copy of what the sweep needs, then let the buffer go.
	 *
	 * The loops below sleep for the pace interval between blocks --
	 * hundreds of milliseconds, sometimes seconds -- and reading
	 * raw->i_direct[i] after each nap means holding a pointer into a
	 * block-device buffer across all of it. The buffer can be
	 * recycled in that window, and then the pointers read out of it
	 * are whatever now occupies the page: brelse on a buffer that was
	 * already free, from beamfs_scrub_check_block, WARNING at
	 * fs/buffer.c:1141.
	 *
	 * Ninety-six bytes of pointers, copied once. The sweep works from
	 * a snapshot, which is what it wants anyway -- a file being
	 * rewritten underneath it is not something the scrubber has to
	 * follow.
	 */
	{
		struct beamfs_inode snap;

		memcpy(&snap, raw, sizeof(snap));
		brelse(bh);
		bh = NULL;
		raw = &snap;
		mode = le16_to_cpu(snap.i_mode);

	/*
	 * Regular files only. A directory block has its own layout, and a
	 * fast symlink keeps its target in the pointer array as raw bytes
	 * -- feeding either to the decoder is the same mistake as sweeping
	 * the bitmap.
	 */
	if (!S_ISREG(mode))
		return;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS && !kthread_should_stop(); i++) {
		u64 blk = le64_to_cpu(raw->i_direct[i]);
		unsigned int corrected = 0;
		int ret;

		if (!blk || !beamfs_block_is_allocated(sb, blk))
			continue;

		ret = beamfs_scrub_check_block(sb, blk, &corrected);
		sbi->s_scrub_blocks++;
		if (corrected)
			sbi->s_scrub_corrected++;
		if (ret == -EUCLEAN)
			sbi->s_scrub_uncorrectable++;

		msleep_interruptible(READ_ONCE(sbi->s_scrub_interval_ms));
	}

	beamfs_scrub_walk_level(sb, le64_to_cpu(raw->i_indirect), 1, sbi);
	beamfs_scrub_walk_level(sb, le64_to_cpu(raw->i_dindirect), 2, sbi);
	beamfs_scrub_walk_level(sb, le64_to_cpu(raw->i_tindirect), 3, sbi);
	}
}

/*
 * Set the pace from what the sweep just found.
 *
 * One rule, applied once per sweep: corrections mean go faster, silence
 * means drift back. Halve the interval on any correction, and add an
 * eighth back when there were none.
 *
 * Halving is deliberate. A rise in flux is abrupt -- a beam turns on,
 * an aircraft reaches altitude -- and a scrubber that eased into it
 * would spend the interesting minutes still at its idle rate. Recovery
 * is gradual for the opposite reason: nothing is lost by staying alert
 * a while after the last correction, and a volume that oscillates
 * around the threshold should settle rather than thrash.
 *
 * Bounded below at 1 ms, because faster is a busy loop rather than a
 * scrub, and above by whatever the operator set, because they know
 * what the volume is for and this only ever tightens their number.
 *
 * The rate is a response to measured corrections, not a prediction. It
 * follows the environment; it does not model it.
 */
static void beamfs_scrub_pace(struct beamfs_sb_info *sbi)
{
	unsigned int cur = READ_ONCE(sbi->s_scrub_interval_ms);
	unsigned int base = sbi->s_scrub_base_ms;
	u64 found;

	if (!cur || !base)
		return;

	found = sbi->s_scrub_corrected - sbi->s_scrub_last_corrected;
	sbi->s_scrub_last_corrected = sbi->s_scrub_corrected;

	if (found) {
		cur = max_t(unsigned int, 1u, cur / 2);
	} else if (cur < base) {
		cur += max_t(unsigned int, 1u, base / 8);
		cur = min(cur, base);
	}

	WRITE_ONCE(sbi->s_scrub_interval_ms, cur);
}

/*
 * What the pacing is doing, and why.
 *
 * A mechanism that changes its own rate has to be able to account for
 * it: an operator seeing the scrubber consume more bandwidth than they
 * configured needs to find the reason here rather than guess. The
 * ratio to base is the useful number -- 1 means quiet, 32 means the
 * volume has been correcting for a while.
 */
static ssize_t scrub_pace_show(struct beamfs_sb_info *sbi, char *buf)
{
	unsigned int cur = READ_ONCE(sbi->s_scrub_interval_ms);
	unsigned int base = sbi->s_scrub_base_ms;

	return sysfs_emit(buf,
			  "interval_ms %u\nbase_ms %u\nfactor %u\ncorrected %llu\nwear_visits %llu\n",
			  cur, base,
			  (cur && base > cur) ? base / cur : 1,
			  sbi->s_scrub_corrected,
			  sbi->s_wear_visits);
}

/*
 * Visit one worn block, if the budget knows of any.
 *
 * The sequential sweep answers "has anything changed"; this answers
 * "where is the margin thin". They are different questions and the
 * pacing above cannot serve the second: raising the rate for the whole
 * volume to watch three blocks doubles the cost of 262051 that are
 * untouched, which is what the measurement showed it doing.
 *
 * Called once per sweep step, so a worn block is seen every few
 * hundred milliseconds while a fresh one waits for the sweep to reach
 * it -- minutes or hours apart on a real volume. The ratio comes out of
 * the geometry rather than a tuning knob: there is one wear visit per
 * ordinary visit, and the number of worn blocks is small by definition.
 * If it stops being small, the volume has a bigger problem than
 * scheduling.
 *
 * Threshold at half the correctable symbols. Below that a block has
 * consumed margin but retains most of it; at four of eight it is closer
 * to the edge than to the start, and that is the point where seeing it
 * often begins to matter.
 */
#define BEAMFS_WEAR_THRESHOLD  (BEAMFS_RS_PARITY / 4)

static void beamfs_scrub_wear_step(struct super_block *sb,
				   struct beamfs_sb_info *sbi)
{
	unsigned int corrected = 0;
	u64 phys;

	if (!sbi->s_budget_len)
		return;

	phys = beamfs_budget_next_worn(sb, sbi->s_wear_cursor,
				       BEAMFS_WEAR_THRESHOLD);
	if (!phys) {
		/* Round again from the start of the data region. */
		sbi->s_wear_cursor = 0;
		return;
	}

	sbi->s_wear_cursor = phys + 1;
	sbi->s_wear_visits++;

	if (!beamfs_block_is_allocated(sb, phys))
		return;

	if (beamfs_scrub_check_block(sb, phys, &corrected) == -EUCLEAN)
		sbi->s_scrub_uncorrectable++;
	sbi->s_scrub_blocks++;
	if (corrected)
		sbi->s_scrub_corrected++;
}

static int beamfs_scrub_thread(void *data)
{
	struct super_block *sb = data;
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	while (!kthread_should_stop()) {
		unsigned long ino;

		if (!READ_ONCE(sbi->s_scrub_interval_ms)) {
			set_current_state(TASK_INTERRUPTIBLE);
			if (!kthread_should_stop())
				schedule_timeout(HZ);
			__set_current_state(TASK_RUNNING);
			continue;
		}

		ino = (unsigned long)sbi->s_scrub_cursor + 1;

		/*
		 * s_inode_bitmap marks inodes that are FREE: setup_bitmap
		 * sets the bit when i_mode reads zero. An allocated inode
		 * is one whose bit is clear, which is the opposite of the
		 * obvious reading and why the first version swept nothing
		 * -- it visited only empty slots and found no data blocks
		 * on any of them.
		 */
		if (sbi->s_inode_bitmap &&
		    !test_bit(ino, sbi->s_inode_bitmap))
			beamfs_scrub_one_inode(sb, ino);

		/*
		 * One worn block per step, alongside the sweep. The two
		 * run at the same cadence but cover sets of wildly
		 * different size, which is what gives worn blocks their
		 * attention without slowing the sweep.
		 */
		beamfs_scrub_wear_step(sb, sbi);

		sbi->s_scrub_cursor++;
		if (sbi->s_scrub_cursor >= sbi->s_ninodes) {
			sbi->s_scrub_cursor = 0;
			sbi->s_scrub_passes++;
			beamfs_scrub_pace(sbi);
			/*
			 * Re-anchor once per sweep, so the wall-clock
			 * reference is at most one sweep old rather than as
			 * old as the mount. A volume left mounted for six
			 * months would otherwise date its events by a
			 * reading taken six months earlier.
			 *
			 * Once per sweep rather than on a timer: a sweep is
			 * already the unit in which this thread thinks, and
			 * a superblock write per sweep is nothing against
			 * the reads the sweep just did.
			 */
			beamfs_clock_anchor(sb);
			beamfs_alert_check_rate(sb);
			/*
			 * A pass over an idle volume costs one inode-table
			 * read per inode and nothing else. Pause between
			 * sweeps so an empty filesystem does not spin.
			 */
			msleep_interruptible(1000);
		} else {
			msleep_interruptible(READ_ONCE(sbi->s_scrub_interval_ms));
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs                                                              */
/* ------------------------------------------------------------------ */

struct beamfs_scrub_attr {
	struct attribute attr;
	ssize_t (*show)(struct beamfs_sb_info *sbi, char *buf);
	ssize_t (*store)(struct beamfs_sb_info *sbi, const char *buf,
			 size_t len);
};

static ssize_t interval_show(struct beamfs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(sbi->s_scrub_interval_ms));
}

static ssize_t interval_store(struct beamfs_sb_info *sbi, const char *buf,
			      size_t len)
{
	unsigned int v;

	if (kstrtouint(buf, 10, &v))
		return -EINVAL;
	/*
	 * Zero parks the thread. There is no upper clamp: an operator who
	 * wants one block a minute on a nearly idle archive is making a
	 * reasonable choice, and one who wants a tight sweep before a
	 * mission is making another.
	 */
	/*
	 * Writing the interval sets the ceiling as well as the current
	 * pace: an operator asking for a rate is stating what the volume
	 * should cost when nothing is wrong, and the pacing only ever
	 * tightens from there.
	 */
	sbi->s_scrub_base_ms = v;
	WRITE_ONCE(sbi->s_scrub_interval_ms, v);
	return len;
}

static ssize_t cursor_show(struct beamfs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%llu\n", sbi->s_scrub_cursor);
}

static ssize_t passes_show(struct beamfs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%llu\n", sbi->s_scrub_passes);
}

static ssize_t blocks_show(struct beamfs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%llu\n", sbi->s_scrub_blocks);
}

static ssize_t corrected_show(struct beamfs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%llu\n", sbi->s_scrub_corrected);
}

static ssize_t uncorrectable_show(struct beamfs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%llu\n", sbi->s_scrub_uncorrectable);
}

/*
 * error_budget -- how much correction capacity the volume has left.
 *
 * One line per wear level: the number of blocks whose worst subblock
 * has needed that many corrections. Level 0 is untouched, level
 * BEAMFS_ERROR_BUDGET_MAX is blocks with no margin at all -- the next
 * upset in the wrong subblock takes them out.
 *
 * Reading walks the whole region, 244 MiB on a 931 GiB volume, so this
 * is a deliberate cost paid on request rather than a counter kept in
 * memory. A counter would have to be rebuilt at mount and would drift
 * against the medium in between; the medium is the record.
 */
static ssize_t error_budget_show(struct beamfs_sb_info *sbi, char *buf)
{
	u64 hist[BEAMFS_ERROR_BUDGET_MAX + 1];
	struct super_block *sb = sbi->s_sb;
	unsigned int i;
	int len = 0;

	if (!sb)
		return sysfs_emit(buf, "unavailable\n");

	if (!sbi->s_budget_blk)
		return sysfs_emit(buf, "disabled\n");

	beamfs_budget_histogram(sb, hist);

	for (i = 0; i <= BEAMFS_ERROR_BUDGET_MAX; i++)
		len += sysfs_emit_at(buf, len, "%u %llu\n", i, hist[i]);

	return len;
}

static ssize_t alert_rate_limit_show(struct beamfs_sb_info *sbi, char *buf)
{
	return sysfs_emit(buf, "%u\n", sbi->s_alert_rate_limit);
}

/*
 * How many blocks may be corrected in one sweep before it is worth
 * telling somebody.
 *
 * Zero disables the check and is the default: the right number depends
 * entirely on where the machine sits, and a value invented here would
 * either cry wolf on a spacecraft or stay silent in a hospital.
 */
static ssize_t alert_rate_limit_store(struct beamfs_sb_info *sbi,
				      const char *buf, size_t len)
{
	u32 v;

	if (kstrtouint(buf, 10, &v))
		return -EINVAL;
	sbi->s_alert_rate_limit = v;
	return len;
}

/*
 * clock_anchor -- what the two clocks read at the same instant.
 *
 * Three fields: the monotonic stamp, the wall time it corresponded to,
 * and what that wall time was worth. A reader converting a journal
 * entry to a date needs all three, and dropping the third turns a
 * qualified statement into an unqualified one.
 */
static ssize_t clock_anchor_show(struct beamfs_sb_info *sbi, char *buf)
{
	static const char * const q[] = {
		"unknown", "rtc", "ntp", "hardware"
	};
	u32 i = sbi->s_anchor_quality;

	if (i >= ARRAY_SIZE(q))
		i = 0;

	return sysfs_emit(buf, "mono %llu\nreal %llu\nquality %s\n",
			  sbi->s_anchor_mono, sbi->s_anchor_real, q[i]);
}

#define BEAMFS_SCRUB_RO(_name) \
	static struct beamfs_scrub_attr beamfs_scrub_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _name##_show, \
	}

#define BEAMFS_SCRUB_RW(_name) \
	static struct beamfs_scrub_attr beamfs_scrub_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0644 }, \
		.show = _name##_show, .store = _name##_store, \
	}

BEAMFS_SCRUB_RW(interval);
BEAMFS_SCRUB_RO(cursor);
BEAMFS_SCRUB_RO(passes);
BEAMFS_SCRUB_RO(blocks);
BEAMFS_SCRUB_RO(corrected);
BEAMFS_SCRUB_RO(uncorrectable);
BEAMFS_SCRUB_RO(error_budget);
BEAMFS_SCRUB_RO(clock_anchor);
BEAMFS_SCRUB_RO(scrub_pace);
BEAMFS_SCRUB_RW(alert_rate_limit);

static struct attribute *beamfs_scrub_attrs[] = {
	&beamfs_scrub_attr_interval.attr,
	&beamfs_scrub_attr_cursor.attr,
	&beamfs_scrub_attr_passes.attr,
	&beamfs_scrub_attr_blocks.attr,
	&beamfs_scrub_attr_corrected.attr,
	&beamfs_scrub_attr_uncorrectable.attr,
	&beamfs_scrub_attr_error_budget.attr,
	&beamfs_scrub_attr_clock_anchor.attr,
	&beamfs_scrub_attr_scrub_pace.attr,
	&beamfs_scrub_attr_alert_rate_limit.attr,
	NULL,
};
ATTRIBUTE_GROUPS(beamfs_scrub);

static ssize_t beamfs_scrub_attr_show(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	struct beamfs_sb_info *sbi =
		container_of(kobj, struct beamfs_sb_info, s_kobj);
	struct beamfs_scrub_attr *a =
		container_of(attr, struct beamfs_scrub_attr, attr);

	return a->show ? a->show(sbi, buf) : -EIO;
}

static ssize_t beamfs_scrub_attr_store(struct kobject *kobj,
				       struct attribute *attr,
				       const char *buf, size_t len)
{
	struct beamfs_sb_info *sbi =
		container_of(kobj, struct beamfs_sb_info, s_kobj);
	struct beamfs_scrub_attr *a =
		container_of(attr, struct beamfs_scrub_attr, attr);

	return a->store ? a->store(sbi, buf, len) : -EIO;
}

static const struct sysfs_ops beamfs_scrub_sysfs_ops = {
	.show  = beamfs_scrub_attr_show,
	.store = beamfs_scrub_attr_store,
};

static void beamfs_scrub_release(struct kobject *kobj)
{
	struct beamfs_sb_info *sbi =
		container_of(kobj, struct beamfs_sb_info, s_kobj);

	complete(&sbi->s_kobj_unregister);
}

static const struct kobj_type beamfs_scrub_ktype = {
	.default_groups = beamfs_scrub_groups,
	.sysfs_ops      = &beamfs_scrub_sysfs_ops,
	.release        = beamfs_scrub_release,
};

static struct kset *beamfs_kset;

int beamfs_scrub_init(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	int ret;

	sbi->s_scrub_interval_ms = READ_ONCE(scrub_interval_ms);
	sbi->s_scrub_base_ms = READ_ONCE(scrub_interval_ms);
	sbi->s_scrub_last_corrected = 0;
	sbi->s_wear_cursor = 0;
	sbi->s_wear_visits = 0;
	sbi->s_scrub_cursor = 0;
	sbi->s_scrub_passes = 0;
	sbi->s_scrub_blocks = 0;
	sbi->s_scrub_corrected = 0;
	sbi->s_scrub_uncorrectable = 0;
	init_completion(&sbi->s_kobj_unregister);

	if (!beamfs_kset) {
		beamfs_kset = kset_create_and_add("beamfs", NULL, fs_kobj);
		if (!beamfs_kset)
			return -ENOMEM;
	}

	sbi->s_kobj.kset = beamfs_kset;
	ret = kobject_init_and_add(&sbi->s_kobj, &beamfs_scrub_ktype,
				   NULL, "%s", sb->s_id);
	if (ret) {
		kobject_put(&sbi->s_kobj);
		wait_for_completion(&sbi->s_kobj_unregister);
		return ret;
	}

	sbi->s_scrub_thread = kthread_run(beamfs_scrub_thread, sb,
					  "beamfs-scrub/%s", sb->s_id);
	if (IS_ERR(sbi->s_scrub_thread)) {
		ret = PTR_ERR(sbi->s_scrub_thread);
		sbi->s_scrub_thread = NULL;
		kobject_del(&sbi->s_kobj);
		kobject_put(&sbi->s_kobj);
		wait_for_completion(&sbi->s_kobj_unregister);
		return ret;
	}

	return 0;
}

void beamfs_scrub_exit(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (!sbi)
		return;

	if (sbi->s_scrub_thread) {
		kthread_stop(sbi->s_scrub_thread);
		sbi->s_scrub_thread = NULL;
	}
	if (sbi->s_kobj.state_initialized) {
		kobject_del(&sbi->s_kobj);
		kobject_put(&sbi->s_kobj);
		wait_for_completion(&sbi->s_kobj_unregister);
	}
}
