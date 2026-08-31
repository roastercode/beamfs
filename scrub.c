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
	int  *results;
	int  *positions;
	u8   *staging;
	unsigned int i, n_corrected = 0;
	int ret = 0;

	*corrected = 0;

	results = kcalloc(BEAMFS_DATA_INLINE_SUBBLOCKS, sizeof(*results),
			  GFP_NOFS);
	positions = kcalloc(BEAMFS_DATA_INLINE_SUBBLOCKS *
			    (BEAMFS_RS_PARITY / 2), sizeof(*positions),
			    GFP_NOFS);
	staging = kmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
	if (!results || !positions || !staging) {
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
	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy(staging + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       (u8 *)bh->b_data + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       BEAMFS_SUBBLOCK_DATA);

	ret = beamfs_rs_decode_region(staging, BEAMFS_SUBBLOCK_DATA,
				      (u8 *)bh->b_data + BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_SUBBLOCK_TOTAL,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS,
				      results, positions,
				      BEAMFS_RS_PARITY / 2);

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		if (results[i] > 0) {
			n_corrected++;
			beamfs_log_rs_event(sb,
				phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
				positions + (size_t)i * (BEAMFS_RS_PARITY / 2),
				(unsigned int)results[i],
				BEAMFS_SUBBLOCK_DATA);
			/*
			 * The sweep is where the budget earns its keep: it
			 * visits blocks nobody reads, which are the ones
			 * that drift unobserved.
			 */
			beamfs_budget_record(sb, phys,
					     (unsigned int)results[i]);
		} else if (results[i] < 0) {
			/*
			 * Past the radius. Journalled with the same shape
			 * the read path uses, so a forensic pass cannot tell
			 * whether a reader or the scrubber found it -- which
			 * is correct: the damage is the same either way.
			 */
			beamfs_log_rs_event_flagged(sb,
				phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
				NULL, 0, BEAMFS_SUBBLOCK_DATA, 0);
			ret = -EUCLEAN;
		}
	}

	*corrected = n_corrected;
	brelse(bh);

out_free:
	kfree(staging);
	kfree(positions);
	kfree(results);
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
	u64 visited = 0;
	u64 j;

	if (!blk || !beamfs_block_is_allocated(sb, blk))
		return 0;

	ibh = sb_bread(sb, blk);
	if (!ibh)
		return 0;

	for (j = 0; j < nptrs && !kthread_should_stop(); j++) {
		__le64 *ptrs = (__le64 *)ibh->b_data;
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
		visited++;
		sbi->s_scrub_blocks++;
		if (corrected)
			sbi->s_scrub_corrected++;
		if (ret == -EUCLEAN)
			sbi->s_scrub_uncorrectable++;

		msleep_interruptible(READ_ONCE(sbi->s_scrub_interval_ms));
	}

	brelse(ibh);
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
	 * Regular files only. A directory block has its own layout, and a
	 * fast symlink keeps its target in the pointer array as raw bytes
	 * -- feeding either to the decoder is the same mistake as sweeping
	 * the bitmap.
	 */
	if (!S_ISREG(mode)) {
		brelse(bh);
		return;
	}

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

	brelse(bh);
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

		sbi->s_scrub_cursor++;
		if (sbi->s_scrub_cursor >= sbi->s_ninodes) {
			sbi->s_scrub_cursor = 0;
			sbi->s_scrub_passes++;
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

static struct attribute *beamfs_scrub_attrs[] = {
	&beamfs_scrub_attr_interval.attr,
	&beamfs_scrub_attr_cursor.attr,
	&beamfs_scrub_attr_passes.attr,
	&beamfs_scrub_attr_blocks.attr,
	&beamfs_scrub_attr_corrected.attr,
	&beamfs_scrub_attr_uncorrectable.attr,
	&beamfs_scrub_attr_error_budget.attr,
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

	sbi->s_scrub_interval_ms = BEAMFS_SCRUB_DEFAULT_INTERVAL_MS;
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
