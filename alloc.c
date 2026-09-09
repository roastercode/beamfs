// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Block and inode allocator
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Both block and inode allocators use in-memory bitmaps loaded at mount
 * time. No I/O is performed under the spinlock.
 *
 * Layout assumption (from mkfs.beamfs):
 *   Block 0          : superblock
 *   Block 1..N       : inode table
 *   Block N+1        : root dir data
 *   Block N+2..end   : data blocks
 *
 * Bitmap convention: bit set (1) = free, bit clear (0) = used.
 *
 * NOTE: on-disk bitmap blocks are planned for v4. Currently the bitmaps
 * are reconstructed at mount by scanning the inode table and from the
 * superblock free_blocks counter. A power-loss between alloc and writeback
 * can leave the superblock counter inconsistent; fsck.beamfs will fix this.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "beamfs.h"
#include "beamfs_trace.h"



/* ------------------------------------------------------------------ */
/* Block bitmap                                                        */
/* ------------------------------------------------------------------ */

/*
 * beamfs_setup_bitmap - allocate and initialize in-memory bitmaps
 *
 * Called from beamfs_fill_super() after the superblock is read.
 *
 * Block bitmap: loaded from the on-disk bitmap block (s_bitmap_blk).
 * Each 239-byte subblock is RS(255,239) FEC-protected. If a subblock
 * is corrected, the event is logged to the Electromagnetic Resilience Journal and
 * the corrected bitmap is written back immediately.
 *
 * Inode bitmap: reconstructed by scanning the inode table for free slots
 * (i_mode == 0). This is O(total_inodes) but only at mount.
 */
int beamfs_setup_bitmap(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long total_blocks;
	unsigned long data_start;
	unsigned long total_inodes;
	unsigned long inode_table_blk;
	unsigned long inodes_per_block;
	unsigned long block, i;
	struct buffer_head *bh;
	struct beamfs_inode *raw;
	u64 bitmap_blk;
	u8 *bdata;
	bool corrected = false;

	/* --- Block bitmap --- */
	total_blocks = le64_to_cpu(sbi->s_beamfs_sb->s_block_count);
	data_start   = le64_to_cpu(sbi->s_beamfs_sb->s_data_start_blk);
	bitmap_blk   = le64_to_cpu(sbi->s_beamfs_sb->s_bitmap_blk);

	if (total_blocks <= data_start) {
		pr_err("beamfs: invalid block layout (total=%lu data_start=%lu)\n",
		       total_blocks, data_start);
		return -EINVAL;
	}

	if (bitmap_blk == 0 || bitmap_blk >= data_start) {
		pr_err("beamfs: invalid bitmap block %llu\n", bitmap_blk);
		return -EINVAL;
	}

	sbi->s_nblocks    = total_blocks - data_start;
	sbi->s_data_start = data_start;

	/*
	 * kvmalloc rather than bitmap_zalloc: one bit per data block means
	 * a 931 GiB volume asks for 29 MiB in one piece, and the page
	 * allocator has no run of contiguous pages that long once the
	 * machine has been up a while. The mount failed with ENOMEM on a
	 * VM with 1.7 GiB free -- not for want of memory, but for want of
	 * it in one stretch.
	 *
	 * vmalloc backing costs an extra page-table walk per access, which
	 * for a bitmap consulted under a spinlock is not measurable
	 * against the block I/O it guards. ext4 makes the same trade for
	 * its larger structures.
	 */
	sbi->s_block_bitmap = kvzalloc_objs(*sbi->s_block_bitmap,
					    BITS_TO_LONGS(sbi->s_nblocks),
					    GFP_KERNEL);
	if (!sbi->s_block_bitmap)
		return -ENOMEM;

	/*
	 * Multi-block bitmap layout: K = bitmap_blocks_count contiguous
	 * blocks starting at bitmap_blk. Each block protects 30592 data
	 * block bits via RS over BEAMFS_BITMAP_SUBBLOCKS (16) sub-blocks.
	 * K is decoded from s_flags bits 0..15; 0 means legacy 1.
	 */
	sbi->s_bitmap_blocks_count =
		beamfs_bitmap_blocks_count_from_flags(sbi->s_beamfs_sb->s_flags);

	sbi->s_bitmap_needs_encode =
		bitmap_zalloc(sbi->s_bitmap_blocks_count, GFP_KERNEL);
	if (!sbi->s_bitmap_needs_encode)
		return -ENOMEM;

	sbi->s_bitmap_blkhs = kcalloc(sbi->s_bitmap_blocks_count,
				      sizeof(*sbi->s_bitmap_blkhs), GFP_KERNEL);
	if (!sbi->s_bitmap_blkhs) {
		kvfree(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
		return -ENOMEM;
	}

	{
		u32 k;
		unsigned long bit_global = 0;
		unsigned long max_bit = sbi->s_nblocks;

		for (k = 0;
		     k < sbi->s_bitmap_blocks_count && bit_global < max_bit;
		     k++) {
			u64 disk_blk = bitmap_blk + (u64)k;
			int rs_results[BEAMFS_BITMAP_SUBBLOCKS];
			int rs_positions[BEAMFS_BITMAP_SUBBLOCKS *
					 (BEAMFS_RS_PARITY / 2)];
			unsigned long bit_local;

			bh = beamfs_bread(sb, disk_blk, "bitmap");
			if (!bh) {
				pr_err("beamfs: cannot read bitmap blk %llu (k=%u)\n",
				       disk_blk, k);
				while (k > 0) {
					k--;
					brelse(sbi->s_bitmap_blkhs[k]);
					sbi->s_bitmap_blkhs[k] = NULL;
				}
				kfree(sbi->s_bitmap_blkhs);
				sbi->s_bitmap_blkhs = NULL;
				bitmap_free(sbi->s_bitmap_needs_encode);
				sbi->s_bitmap_needs_encode = NULL;
				kvfree(sbi->s_block_bitmap);
				sbi->s_block_bitmap = NULL;
				return -EIO;
			}
			sbi->s_bitmap_blkhs[k] = bh;
			bdata = (u8 *)bh->b_data;

			beamfs_rs_decode_region(
				bdata, BEAMFS_SUBBLOCK_TOTAL,
				bdata + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_BITMAP_SUBBLOCKS,
				rs_results, rs_positions,
				BEAMFS_RS_PARITY / 2,
				"bitmap");

			for (i = 0; i < BEAMFS_BITMAP_SUBBLOCKS; i++) {
				int rc = rs_results[i];

				if (rc < 0) {
					pr_err("beamfs: bmap blk %u sub %lu uncor\n",
					       k, i);
				} else if (rc > 0) {
					unsigned int np = (unsigned int)rc;
					int *pos = rs_positions +
						i * (BEAMFS_RS_PARITY / 2);

					if (np > BEAMFS_RS_PARITY / 2)
						np = BEAMFS_RS_PARITY / 2;
					pr_warn("beamfs: bmap blk %u sub %lu: %d corrected\n",
						k, i, rc);
					beamfs_log_rs_event(sb,
						disk_blk * BEAMFS_BITMAP_SUBBLOCKS + i,
						pos, np,
						BEAMFS_SUBBLOCK_DATA);
					corrected = true;
				}
			}

			bit_local = 0;
			for (i = 0;
			     i < BEAMFS_BITMAP_SUBBLOCKS &&
			     bit_local < BEAMFS_BITS_PER_BITMAP_BLOCK &&
			     bit_global < max_bit; i++) {
				u8 *subdata = bdata + i * BEAMFS_SUBBLOCK_TOTAL;
				unsigned long b;

				for (b = 0;
				     b < BEAMFS_SUBBLOCK_DATA * 8 &&
				     bit_global < max_bit;
				     b++, bit_local++, bit_global++) {
					if (subdata[b / 8] & (1u << (b % 8)))
						set_bit(bit_global,
							sbi->s_block_bitmap);
					else
						clear_bit(bit_global,
							  sbi->s_block_bitmap);
				}
			}

			if (corrected) {
				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				corrected = false;
			}
		}
	}

	/* --- Inode bitmap --- */
	total_inodes     = le64_to_cpu(sbi->s_beamfs_sb->s_inode_count);
	inode_table_blk  = le64_to_cpu(sbi->s_beamfs_sb->s_inode_table_blk);
	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);

	/*
	 * Start at the beginning: at mount the layout is whatever the
	 * previous session left, and there is no better guess.
	 */
	sbi->s_alloc_goal = 0;

	sbi->s_ninodes = total_inodes;

	sbi->s_inode_bitmap = kvzalloc_objs(*sbi->s_inode_bitmap,
					    BITS_TO_LONGS(total_inodes + 1),
					    GFP_KERNEL);
	if (!sbi->s_inode_bitmap) {
		u32 k;

		for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
			if (sbi->s_bitmap_blkhs[k])
				brelse(sbi->s_bitmap_blkhs[k]);
		}
		kfree(sbi->s_bitmap_blkhs);
		sbi->s_bitmap_blkhs = NULL;
		bitmap_free(sbi->s_bitmap_needs_encode);
		sbi->s_bitmap_needs_encode = NULL;
		kvfree(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
		return -ENOMEM;
	}

	for (block = 0; block * inodes_per_block < total_inodes; block++) {
		bh = beamfs_bread(sb, inode_table_blk + block, "bitmap");
		if (!bh) {
			pr_warn("beamfs: cannot read inode table block %lu at mount\n",
				inode_table_blk + block);
			continue;
		}

		raw = (struct beamfs_inode *)bh->b_data;

		for (i = 0; i < inodes_per_block; i++) {
			unsigned long ino = block * inodes_per_block + i + 1;

			if (ino > total_inodes)
				break;
			if (ino == 1)
				continue;
			if (le16_to_cpu(raw[i].i_mode) == 0)
				set_bit(ino, sbi->s_inode_bitmap);
		}
		brelse(bh);
	}

	pr_info("beamfs: bitmaps initialized (%lu data blocks, %lu free; "
		"%lu inodes, %lu free)\n",
		sbi->s_nblocks, sbi->s_free_blocks,
		total_inodes, sbi->s_free_inodes);

	return 0;
}

/*
 * beamfs_write_bitmap_block - flush a SINGLE on-disk bitmap block to disk
 *                             with RS FEC, for the bitmap block that
 *                             contains @bit_global.
 *
 * Re-encoding all on-disk bitmap blocks on every allocator state change
 * was historically the source of fsync latency proportional to the
 * volume size (each beamfs_alloc_block under s_lock used to redo
 * memset+RS-encode+mark_buffer_dirty on every bitmap block, ~52 blocks
 * for a 1 GiB volume). Since each bitmap block is RS-protected
 * independently, only the block that contains the modified bit needs
 * to be re-encoded and re-marked dirty.
 *
 * Called under s_lock. @bit_global is the global bit index (0-based)
 * just modified in sbi->s_block_bitmap by the caller.
 */
/*
 * Rebuild the on-disk image of every bitmap block whose bits moved.
 *
 * Called from sync_fs, immediately before the buffers are written, so
 * one rebuild covers however many allocations and frees happened since
 * the last sync -- 220000 of them for an 800 MiB write, previously one
 * rebuild each.
 *
 * s_block_bitmap is the authority and is read under test_bit, which is
 * atomic; the buffer lock serialises against a concurrent rebuild of
 * the same block.
 */
/*
 * Rebuild one bitmap block's on-disk image. The buffer lock must be held.
 */
/*
 * Rebuild one subblock of a bitmap block, or all of them.
 *
 * @sub: which codeword to rebuild, or BEAMFS_BITMAP_SUBBLOCKS for all.
 *
 * The whole-block version cost 30592 test_bit calls and sixteen RS
 * encodes for one bit changed. Filling 256 MiB means 70197 allocations,
 * so generic/015 spent two billion test_bit calls and 1.1 million RS
 * encodes in the allocator -- twenty minutes on a test that should take
 * one, with the flusher pinned at 92% of a core.
 *
 * Two things were wasteful and neither had to be. A bit belongs to
 * exactly one of the sixteen codewords, so changing it invalidates one
 * parity block, not sixteen. And the in-memory bitmap is already a bit
 * array in the same order as the on-disk one: memcpy moves it, where
 * the old loop asked about every bit individually.
 *
 * Per allocation this becomes one memcpy of 239 bytes and one RS
 * encode. The full-block path stays for mount, where the whole image
 * genuinely has to be built.
 */
static void beamfs_bitmap_encode_sub_locked(struct beamfs_sb_info *sbi,
					    u32 k, u32 sub,
					    struct buffer_head *bh)
{
	unsigned long first, last;
	u8 *bdata = (u8 *)bh->b_data;
	u32 i, lo, hi;

	first = (unsigned long)k * BEAMFS_BITS_PER_BITMAP_BLOCK;
	last  = min(first + BEAMFS_BITS_PER_BITMAP_BLOCK,
		    (unsigned long)sbi->s_nblocks);

	if (sub >= BEAMFS_BITMAP_SUBBLOCKS) {
		lo = 0;
		hi = BEAMFS_BITMAP_SUBBLOCKS;
		memset(bdata, 0, BEAMFS_BLOCK_SIZE);
	} else {
		lo = sub;
		hi = sub + 1;
	}

	for (i = lo; i < hi; i++) {
		u8 *subdata = bdata + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;
		unsigned long bit0 = first +
				     (unsigned long)i * BEAMFS_SUBBLOCK_DATA * 8;
		size_t nbytes;

		if (bit0 >= last) {
			memset(subdata, 0, BEAMFS_SUBBLOCK_DATA);
			continue;
		}

		/*
		 * Bit b of the volume lives at bit b - bit0 of this
		 * codeword, and bit0 is a multiple of 1912 -- which is a
		 * multiple of 8, so the copy is byte-aligned and memcpy
		 * does it.
		 */
		nbytes = min_t(size_t, BEAMFS_SUBBLOCK_DATA,
			       DIV_ROUND_UP(last - bit0, 8));
		memcpy(subdata, (const u8 *)sbi->s_block_bitmap + bit0 / 8,
		       nbytes);
		if (nbytes < BEAMFS_SUBBLOCK_DATA)
			memset(subdata + nbytes, 0,
			       BEAMFS_SUBBLOCK_DATA - nbytes);

		beamfs_rs_encode_region(subdata, BEAMFS_SUBBLOCK_TOTAL,
					subdata + BEAMFS_SUBBLOCK_DATA,
					BEAMFS_SUBBLOCK_TOTAL,
					BEAMFS_SUBBLOCK_DATA, 1);
	}
}

static void beamfs_bitmap_encode_one_locked(struct beamfs_sb_info *sbi, u32 k,
					    struct buffer_head *bh)
{
	beamfs_bitmap_encode_sub_locked(sbi, k, BEAMFS_BITMAP_SUBBLOCKS, bh);
}

void beamfs_bitmap_encode_pending(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u32 k;

	if (!sbi || !sbi->s_bitmap_blkhs || !sbi->s_bitmap_needs_encode ||
	    !sbi->s_block_bitmap)
		return;

	for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
		struct buffer_head *bh;

		if (!test_and_clear_bit(k, sbi->s_bitmap_needs_encode))
			continue;

		bh = sbi->s_bitmap_blkhs[k];
		if (!bh)
			continue;

		lock_buffer(bh);
		beamfs_bitmap_encode_one_locked(sbi, k, bh);
		unlock_buffer(bh);
	}
}

int beamfs_write_bitmap_block(struct super_block *sb,
			      unsigned long bit_global,
			      struct inode *owner)
{
	if (beamfs_failed(sb))
		return -EIO;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct buffer_head *bh;
	u32 k;

	if (!sbi->s_bitmap_blkhs || !sbi->s_block_bitmap)
		return -EINVAL;
	if (bit_global >= sbi->s_nblocks)
		return -EINVAL;

	k = (u32)(bit_global / BEAMFS_BITS_PER_BITMAP_BLOCK);
	if (k >= sbi->s_bitmap_blocks_count)
		return -EINVAL;

	bh = sbi->s_bitmap_blkhs[k];
	if (!bh)
		return -EINVAL;

	/*
	 * Rebuild the block's on-disk image now, under the lock.
	 *
	 * A deferred version of this lived here for a day: mark the block
	 * as needing a rebuild, do it in sync_fs, one rebuild per block
	 * touched instead of one per bit. It measured 25% faster on
	 * writes and a factor of four on deletes, and it was wrong.
	 *
	 * The needs_encode flag and the buffer's dirty bit are two states
	 * and the kernel knows only the second. Between mark_buffer_dirty
	 * below and sync_fs getting round to the rebuild, the flusher can
	 * write the buffer with its old contents and clear dirty; sync_fs
	 * then rebuilds an image nobody will ever write. fsck found
	 * 122368 blocks still marked used after 400 MiB was written and
	 * deleted, doubling with every cycle, while df reported the
	 * volume empty. The bitmap in memory was right and the one on
	 * disk was a lie.
	 *
	 * Three thousand eight hundred bytes walked and sixteen RS
	 * encodes, on every allocation and every free. That is what
	 * correctness costs here. Bringing it down means changing fewer
	 * bits per rebuild, not letting the buffer reach the disk stale.
	 */
	lock_buffer(bh);
	/*
	 * Only the codeword this bit belongs to. The other fifteen are
	 * untouched and their parity still holds.
	 */
	beamfs_bitmap_encode_sub_locked(sbi, k,
		(u32)((bit_global % BEAMFS_BITS_PER_BITMAP_BLOCK) /
		      (BEAMFS_SUBBLOCK_DATA * 8)),
		bh);
	clear_bit(k, sbi->s_bitmap_needs_encode);
	mark_buffer_dirty(bh);

	/*
	 * Bind to the owner inode so VFS writeback flushes this bitmap
	 * block BEFORE the owner inode is marked clean. The VFS flushes
	 * the inode's metadata buffer list in __writeback_single_inode
	 * before write_inode, which establishes the bitmap-before-inode
	 * ordering that prevents the boot-time double-free: without it,
	 * boot N could persist an inode pointer without persisting the
	 * matching bitmap clear, leaving boot N+1 to mount a bitmap
	 * declaring a block free while an inode still referenced it.
	 *
	 * owner is NULL on the mount path, where the caller drives a
	 * synchronous sync_dirty_buffer itself, and on the free path from
	 * beamfs_free_data_blocks.
	 */
	if (owner && !(inode_state_read_once(owner) & I_FREEING))
		mmb_mark_buffer_dirty(bh, &BEAMFS_I(owner)->i_metadata_bhs);

	unlock_buffer(bh);
	return 0;
}

/*
 * beamfs_destroy_bitmap - free in-memory bitmaps at umount
 */
void beamfs_destroy_bitmap(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (sbi->s_bitmap_blkhs) {
		u32 k;

		for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
			if (sbi->s_bitmap_blkhs[k]) {
				brelse(sbi->s_bitmap_blkhs[k]);
				sbi->s_bitmap_blkhs[k] = NULL;
			}
		}
		kfree(sbi->s_bitmap_blkhs);
		sbi->s_bitmap_blkhs = NULL;
		bitmap_free(sbi->s_bitmap_needs_encode);
		sbi->s_bitmap_needs_encode = NULL;
	}
	if (sbi->s_block_bitmap) {
		kvfree(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
	}
	if (sbi->s_inode_bitmap) {
		kvfree(sbi->s_inode_bitmap);
		sbi->s_inode_bitmap = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Block allocation                                                    */
/* ------------------------------------------------------------------ */

/*
 * beamfs_alloc_block - allocate a free data block
 *
 * Returns absolute block number (>= s_data_start) on success,
 * or 0 on failure (block 0 is the superblock, never a valid data block).
 * No I/O performed; bitmap is in memory.
 */
u64 beamfs_alloc_block(struct super_block *sb, struct inode *owner)
{
	/*
	 * A failed volume takes no more writes. See beamfs_fail: the
	 * device is gone, its buffers cannot be read back, and marking
	 * them dirty is what generic/338 catches with dm-error.
	 */
	if (beamfs_failed(sb))
		return 0;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	if (!sbi->s_block_bitmap) {
		pr_err("beamfs: block bitmap not initialized\n");
		return 0;
	}

	spin_lock(&sbi->s_lock);

	if (sbi->s_free_blocks == 0) {
		spin_unlock(&sbi->s_lock);
		return 0;
	}

	/*
	 * From where the last one landed, then from the start.
	 *
	 * Two passes rather than one scan from zero: the first covers the
	 * region a sequential writer is actually using, the second the
	 * holes earlier writers left behind. Together they are exhaustive,
	 * so free_blocks > 0 still guarantees a hit.
	 */
	bit = find_next_bit(sbi->s_block_bitmap, sbi->s_nblocks,
			    sbi->s_alloc_goal);
	if (bit >= sbi->s_nblocks && sbi->s_alloc_goal != 0)
		bit = find_first_bit(sbi->s_block_bitmap, sbi->s_nblocks);

	if (bit >= sbi->s_nblocks) {
		spin_unlock(&sbi->s_lock);
		pr_err("beamfs: bitmap inconsistency: free_blocks=%lu but no free bit\n",
		       sbi->s_free_blocks);
		return 0;
	}

	/*
	 * Next search starts after this one. Past the end it wraps, which
	 * the second pass above then handles.
	 */
	sbi->s_alloc_goal = bit + 1;
	if (sbi->s_alloc_goal >= sbi->s_nblocks)
		sbi->s_alloc_goal = 0;

	clear_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks--;
	sbi->s_beamfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	/* Reconstruct on-disk bitmap block from current s_block_bitmap
	 * RAM state. Done OUTSIDE s_lock because write_bitmap_block now
	 * uses lock_buffer (sleepable). Reading s_block_bitmap via
	 * test_bit is atomic, no lock needed for the reconstruction.
	 */
	beamfs_write_bitmap_block(sb, bit, owner);

	trace_beamfs_block_alloc(owner ? owner->i_ino : 0,
				 (u64)(sbi->s_data_start + bit), 0);
	return (u64)(sbi->s_data_start + bit);
}

/*
 * beamfs_free_block - return a data block to the free pool
 */
/*
 * beamfs_block_is_allocated -- is @block currently in use?
 *
 * Indirect blocks hold 512 pointers in exactly 4096 bytes, leaving no room
 * for a checksum, so unlike data blocks, inodes and the superblock they carry
 * no integrity field of their own. A flip in one corrupts a pointer, and the
 * bounds check at the read sites only rejects values outside the data area.
 * A corrupted pointer that lands inside it reaches an unrelated block and the
 * read proceeds. DATA_SELFID catches the case where that block belongs to
 * another file; this catches the case where it belongs to no file at all,
 * which is what a random flip on a 64-bit pointer usually produces.
 *
 * The in-memory bitmap uses 1 for free, so an allocated block has its bit
 * clear. It is held for the lifetime of the mount and is RS-protected on
 * disk, so consulting it costs a bit test and no I/O.
 *
 * A prior version of this check (2026-08-22) found that mkfs.beamfs never
 * marked root_dir_blk allocated (fixed in yocto-beamfs c2bebac): every
 * root-fs image built with --from-dir failed to boot, because the first
 * pointer resolved at mount, i_direct[0] of the root inode, pointed at a
 * block the bitmap called free. Re-enabled after that fix.
 */
bool beamfs_block_is_allocated(struct super_block *sb, u64 block)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	/*
	 * 2026-08-23: blocks below s_data_start (superblock, inode table,
	 * bitmap, root dir, canary) are reserved and never covered by the
	 * bitmap by construction -- see the layout comment at the top of
	 * this file. Treating them as "not allocated" made every legitimate
	 * pointer into that zone fail, starting with i_direct[0] of the
	 * root inode: the very first block resolved at mount was rejected,
	 * and no image built with --from-dir could boot. mkfs was patched
	 * to mark root_dir_blk in the bitmap to work around this (yocto-beamfs
	 * c2bebac) and that patch has been reverted (846c2bd): the root and
	 * canary blocks are not supposed to be bitmap-covered, matching the
	 * "silently skip reserved blocks" doctrine already applied in
	 * beamfs_free_block below. The reserved zone is bounded above by
	 * s_data_start and below by 0, both already enforced by the bounds
	 * check at every read site (file_inline.c), so no separate check is
	 * needed here for it.
	 */
	if (block < sbi->s_data_start)
		return true;
	if (!sbi->s_block_bitmap)
		return true;   /* bitmap unavailable: do not reject */
	bit = (unsigned long)(block - sbi->s_data_start);
	if (bit >= sbi->s_nblocks)
		return false;

	return !test_bit(bit, sbi->s_block_bitmap);
}

void beamfs_free_block(struct super_block *sb, u64 block, struct inode *owner)
{
	/*
	 * A failed volume takes no more writes. See beamfs_fail: the
	 * device is gone, its buffers cannot be read back, and marking
	 * them dirty is what generic/338 catches with dm-error.
	 */
	if (beamfs_failed(sb))
		return;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	/* Layer 2 defense: silently skip reserved blocks. Any caller
	 * reaching this path with block < s_data_start is either a
	 * legitimate truncate of a reserved-pointing inode (canary, only
	 * possible if Layer 1 S_IMMUTABLE was bypassed by corruption) or
	 * a stale pointer from a corrupted indirect block. In both cases,
	 * silently rejecting preserves bitmap integrity without log spam.
	 */
	if (block < sbi->s_data_start)
		return;

	bit = (unsigned long)(block - sbi->s_data_start);
	if (bit >= sbi->s_nblocks) {
		pr_err("beamfs: block %llu out of range\n", block);
		return;
	}

	spin_lock(&sbi->s_lock);

	if (test_bit(bit, sbi->s_block_bitmap)) {
		/*
		 * Rate-limited, and no stack.
		 *
		 * A double free is worth knowing about, but a stack dump
		 * for one is a debugging aid that becomes a liability the
		 * moment the condition is reachable in normal operation.
		 * Under dm-error -- generic/338 -- writes fail by design,
		 * frees retry, and the trace lands in dmesg where
		 * _check_dmesg finds it and fails a test that was
		 * measuring something else entirely.
		 *
		 * The message still says which block, which is what a
		 * reader needs; whoever wants the caller can set a
		 * kprobe.
		 */
		pr_warn_ratelimited("beamfs: double free of block %llu\n",
				    block);
		spin_unlock(&sbi->s_lock);
		return;
	}
	trace_beamfs_block_free(owner ? owner->i_ino : 0, block, 0);

	/*
	 * Pull the goal back to what was just freed, if it is behind.
	 *
	 * Without this a block freed before the cursor is only found on
	 * the next wrap, so a workload that fills, deletes and refills --
	 * generic/015 and generic/027 both do exactly that -- walks the
	 * whole bitmap again on every cycle instead of reusing what it
	 * just released.
	 */
	if (bit < sbi->s_alloc_goal)
		sbi->s_alloc_goal = bit;

	set_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks++;
	sbi->s_beamfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	/* See alloc_block: reconstruct on-disk bitmap outside lock. */
	/*
	 * A freed block's pointer may legitimately vanish, and a freed
	 * indirect block's slots go with it. Tell the checker before the
	 * block is handed to anybody else, or every ordinary truncate
	 * reads as a violation.
	 */
	beamfs_tc_forget_child(sb, block);
	beamfs_tc_forget_parent(sb, block);
	beamfs_write_bitmap_block(sb, bit, owner);
}

/* ------------------------------------------------------------------ */
/* Inode number allocation                                             */
/* ------------------------------------------------------------------ */

/*
 * beamfs_alloc_inode_num - allocate a free inode number
 *
 * Uses the in-memory inode bitmap. No I/O performed, no sb_bread under
 * spinlock.
 *
 * Returns inode number >= 2 on success (1 = root, always reserved),
 * or 0 on failure.
 */
u64 beamfs_alloc_inode_num(struct super_block *sb)
{
	/*
	 * A failed volume takes no more writes. See beamfs_fail: the
	 * device is gone, its buffers cannot be read back, and marking
	 * them dirty is what generic/338 catches with dm-error.
	 */
	if (beamfs_failed(sb))
		return 0;

	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit;

	if (!sbi->s_inode_bitmap) {
		pr_err("beamfs: inode bitmap not initialized\n");
		return 0;
	}

	spin_lock(&sbi->s_lock);

	if (sbi->s_free_inodes == 0) {
		spin_unlock(&sbi->s_lock);
		return 0;
	}

	/*
	 * Bits 0 and 1 are never set (inode 0 invalid, inode 1 = root reserved).
	 * find_next_bit starting at 2 skips both.
	 */
	bit = find_next_bit(sbi->s_inode_bitmap, sbi->s_ninodes + 1, 2);
	if (bit > sbi->s_ninodes) {
		spin_unlock(&sbi->s_lock);
		pr_err("beamfs: inode bitmap inconsistency: free_inodes=%lu but no free bit\n",
		       sbi->s_free_inodes);
		return 0;
	}

	clear_bit(bit, sbi->s_inode_bitmap);
	sbi->s_free_inodes--;
	sbi->s_beamfs_sb->s_free_inodes = cpu_to_le64(sbi->s_free_inodes);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);

	return (u64)bit;
}

/*
 * beamfs_free_inode_num - return an inode number to the free pool
 *
 * Called from evict_inode path when nlink drops to 0.
 */
void beamfs_free_inode_num(struct super_block *sb, u64 ino)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	/* Layer 2 defense: silently skip reserved inodes (root, canary).
	 * Range check covers both reserved range and bitmap bounds.
	 */
	if (beamfs_ino_is_reserved(ino) || ino > sbi->s_ninodes)
		return;

	spin_lock(&sbi->s_lock);

	if (test_bit((unsigned long)ino, sbi->s_inode_bitmap)) {
		pr_warn("beamfs: double free of inode %llu\n", ino);
		spin_unlock(&sbi->s_lock);
		return;
	}

	set_bit((unsigned long)ino, sbi->s_inode_bitmap);
	sbi->s_free_inodes++;
	sbi->s_beamfs_sb->s_free_inodes = cpu_to_le64(sbi->s_free_inodes);
	beamfs_dirty_super(sbi);

	spin_unlock(&sbi->s_lock);
}
