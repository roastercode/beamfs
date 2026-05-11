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
#include "beamfs.h"

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

	sbi->s_block_bitmap = bitmap_zalloc(sbi->s_nblocks, GFP_KERNEL);
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

	sbi->s_bitmap_blkhs = kcalloc(sbi->s_bitmap_blocks_count,
				      sizeof(*sbi->s_bitmap_blkhs), GFP_KERNEL);
	if (!sbi->s_bitmap_blkhs) {
		bitmap_free(sbi->s_block_bitmap);
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

			bh = sb_bread(sb, disk_blk);
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
				bitmap_free(sbi->s_block_bitmap);
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
				BEAMFS_RS_PARITY / 2);

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

	sbi->s_ninodes = total_inodes;

	sbi->s_inode_bitmap = bitmap_zalloc(total_inodes + 1, GFP_KERNEL);
	if (!sbi->s_inode_bitmap) {
		u32 k;

		for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
			if (sbi->s_bitmap_blkhs[k])
				brelse(sbi->s_bitmap_blkhs[k]);
		}
		kfree(sbi->s_bitmap_blkhs);
		sbi->s_bitmap_blkhs = NULL;
		bitmap_free(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
		return -ENOMEM;
	}

	for (block = 0; block * inodes_per_block < total_inodes; block++) {
		bh = sb_bread(sb, inode_table_blk + block);
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
 * beamfs_write_bitmap - flush in-memory block bitmap to disk with RS FEC
 *
 * Encodes each 239-byte data subblock with 16 bytes of RS parity and
 * marks the bitmap buffer dirty. Called under s_lock.
 */
int beamfs_write_bitmap(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	unsigned long bit_global = 0;
	unsigned long max_bit;
	u32 k;

	if (!sbi->s_bitmap_blkhs || !sbi->s_block_bitmap)
		return -EINVAL;

	max_bit = sbi->s_nblocks;

	for (k = 0; k < sbi->s_bitmap_blocks_count; k++) {
		struct buffer_head *bh = sbi->s_bitmap_blkhs[k];
		u8 *bdata;
		unsigned long bit_local = 0;
		unsigned long i, b;

		if (!bh)
			continue;
		bdata = (u8 *)bh->b_data;
		memset(bdata, 0, BEAMFS_BLOCK_SIZE);

		for (i = 0;
		     i < BEAMFS_BITMAP_SUBBLOCKS &&
		     bit_local < BEAMFS_BITS_PER_BITMAP_BLOCK &&
		     bit_global < max_bit; i++) {
			u8 *subdata = bdata + i * BEAMFS_SUBBLOCK_TOTAL;

			for (b = 0;
			     b < BEAMFS_SUBBLOCK_DATA * 8 &&
			     bit_global < max_bit;
			     b++, bit_local++, bit_global++) {
				if (test_bit(bit_global, sbi->s_block_bitmap))
					subdata[b / 8] |= (1u << (b % 8));
			}
		}

		beamfs_rs_encode_region(
			bdata, BEAMFS_SUBBLOCK_TOTAL,
			bdata + BEAMFS_SUBBLOCK_DATA,
			BEAMFS_SUBBLOCK_TOTAL,
			BEAMFS_SUBBLOCK_DATA,
			BEAMFS_BITMAP_SUBBLOCKS);

		mark_buffer_dirty(bh);
	}
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
	}
	if (sbi->s_block_bitmap) {
		bitmap_free(sbi->s_block_bitmap);
		sbi->s_block_bitmap = NULL;
	}
	if (sbi->s_inode_bitmap) {
		bitmap_free(sbi->s_inode_bitmap);
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
u64 beamfs_alloc_block(struct super_block *sb)
{
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

	bit = find_first_bit(sbi->s_block_bitmap, sbi->s_nblocks);
	if (bit >= sbi->s_nblocks) {
		spin_unlock(&sbi->s_lock);
		pr_err("beamfs: bitmap inconsistency: free_blocks=%lu but no free bit\n",
		       sbi->s_free_blocks);
		return 0;
	}

	clear_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks--;
	sbi->s_beamfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	beamfs_dirty_super(sbi);
	beamfs_write_bitmap(sb);

	spin_unlock(&sbi->s_lock);

	return (u64)(sbi->s_data_start + bit);
}

/*
 * beamfs_free_block - return a data block to the free pool
 */
void beamfs_free_block(struct super_block *sb, u64 block)
{
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
		pr_warn("beamfs: double free of block %llu\n", block);
		dump_stack();
		spin_unlock(&sbi->s_lock);
		return;
	}

	set_bit(bit, sbi->s_block_bitmap);
	sbi->s_free_blocks++;
	sbi->s_beamfs_sb->s_free_blocks = cpu_to_le64(sbi->s_free_blocks);
	beamfs_dirty_super(sbi);
	beamfs_write_bitmap(sb);

	spin_unlock(&sbi->s_lock);
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
