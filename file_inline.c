// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - File operations for BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE (v2)
 *
 * Per-block Reed-Solomon FEC on user data: each 4096-byte disk block
 * holds 16 RS(255,239) shortened subblocks (3824 user bytes + 256 parity
 * + 16 pad bytes).
 *
 * This file implements the data path for scheme=2 (UNIVERSAL_INLINE).
 * The legacy iomap-based path in file.c is preserved for scheme=5
 * (INODE_UNIVERSAL); the dispatch happens in inode.c / namei.c when
 * setting i_fop and a_ops based on sbi->s_scheme.
 *
 * Threat model: validates against RadFI v0.1.0+ (single-bit flip in
 * bio_vec page payload during submit_bio_noacct). MIL-STD-883 SEE
 * coverage: up to 128 bytes corruption per disk block (8 byte symbols
 * per subblock * 16 subblocks).
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/writeback.h>
#include <linux/crc32.h>
#include <linux/unaligned.h>
#include "beamfs.h"
#include "beamfs_trace.h"
#include <linux/iomap.h>

/* ------------------------------------------------------------------------- */
/* Forward declarations of v2 ops (stubs, populated in subsequent stages)    */
/* ------------------------------------------------------------------------- */

static int     beamfs_inline_read_folio(struct file *file,
					struct folio *folio);
static int     beamfs_inline_writepages(struct address_space *mapping,
					struct writeback_control *wbc);
static ssize_t beamfs_inline_file_write_iter(struct kiocb *iocb,
					     struct iov_iter *from);
static int     beamfs_inline_setattr(struct mnt_idmap *idmap,
				    struct dentry *dentry,
				    struct iattr *attr);
static void    beamfs_inline_free_blocks_from(struct inode *inode,
					      u64 b_first_freed);
static void    beamfs_inline_stamp_tail_pad(struct beamfs_sb_info *sbi,
					    u8 *block, const u8 *payload,
					    u64 ino, u64 iblock);
static int     beamfs_inline_zero_tail_block(struct inode *inode,
					     u64 b, u32 zero_offset);
static int     beamfs_check_intermediate_block(struct super_block *sb,
					       u64 block, ino_t ino,
					       u64 iblock_logical,
					       const char *label);

/* ------------------------------------------------------------------------- */
/* Block-mapping helpers (v2 INLINE)                                         */
/*                                                                           */
/* These mirror the layout used by beamfs_iomap_begin() in file.c (legacy    */
/* iomap path, scheme=5 INODE_UNIVERSAL): direct blocks 0..11 in             */
/* fi->i_direct[], single indirect via fi->i_indirect (512 entries) for      */
/* iblocks 12..523. Maximum mapped iblock in v1 layout: 524 -> ~2.0 MiB of   */
/* logical user data per file at 3824 user bytes per disk block.             */
/*                                                                           */
/* Pure lookup, no allocation. Used by read_folio (4b2.2) and as the read    */
/* leg of write_begin (4b3) RMW. The allocating variant lives below          */
/* (added in 4b3).                                                           */
/*                                                                           */
/* Returns:                                                                  */
/*   0  + *phys_out = block number  (mapped)                                 */
/*   0  + *phys_out = 0             (HOLE: not allocated yet)                */
/*   <0 on error                    (-EIO indirect read fail,                */
/*                                   -EOPNOTSUPP beyond v1 capacity,        */
/*                                   -EINVAL on null phys_out)              */
/* ------------------------------------------------------------------------- */
/*
 * beamfs_check_intermediate_block -- bounds and allocation check for a
 * pointer-to-pointers block (indirect, dindirect, tindirect and their L1/L2
 * levels) before it is read as 512 raw __le64 entries.
 *
 * The four existing call sites in this file check the terminal pointer --
 * the one that addresses a leaf data block -- against both the
 * [s_data_start, s_data_start + s_nblocks) range and the allocation bitmap.
 * The intermediate blocks that hold indirection pointers had neither check:
 * a bit flip on fi->i_indirect (or i_dindirect, i_tindirect, or an L1/L2
 * entry) reaching a value inside the valid range but pointing at an
 * unrelated or unallocated block was read via sb_bread and its raw bytes
 * were reinterpreted as 512 pointers, with no signal. This closes that gap
 * the same way DATA_SELFID and beamfs_block_is_allocated closed it for
 * terminal pointers and file data: fail closed before the read rather than
 * trusting whatever the corrupted pointer happens to reach.
 */
static int beamfs_check_intermediate_block(struct super_block *sb,
					   u64 block, ino_t ino,
					   u64 iblock_logical,
					   const char *label)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	if (block < sbi->s_data_start ||
	    block >= sbi->s_data_start + sbi->s_nblocks) {
		pr_err_ratelimited("beamfs/inline: corrupted %s block ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
				   label, (unsigned long long)ino,
				   (unsigned long long)iblock_logical,
				   (unsigned long long)block,
				   sbi->s_data_start,
				   sbi->s_data_start + sbi->s_nblocks);
		beamfs_log_rs_event_flagged(sb, block, NULL, 0,
					    BEAMFS_SUBBLOCK_DATA, 0);
		return -EUCLEAN;
	}
	if (!beamfs_block_is_allocated(sb, block)) {
		/*
		 * Journal it as uncorrectable, same as the DATA_CSUM and
		 * DATA_SELFID fail-closed paths. An indirection pointer
		 * corrupted beyond the RS correction radius is exactly what
		 * the forensic journal exists to record; without an entry
		 * the run reads as an unexplained failure, since dmesg
		 * carries the pr_err but DMESG_UNCORRECTABLE stays 0. That
		 * is how the 2026-08-27 multifs run scored RS_FAILED rather
		 * than RS_FAIL_CLOSED. positions == NULL with
		 * n_positions == 0 is the uncorrectable call shape; the flag
		 * is reserved and set by beamfs_log_rs_event_flagged itself
		 * (format-v4.md section 6.5) -- passing it in extra_flags
		 * would trip the reserved-mask guard, as 66b8492 fixed.
		 */
		beamfs_log_rs_event_flagged(sb, block, NULL, 0,
					    BEAMFS_SUBBLOCK_DATA, 0);
		pr_err_ratelimited("beamfs/inline: unallocated %s block ino=%llu iblock=%llu phys=%llu\n",
				   label, (unsigned long long)ino,
				   (unsigned long long)iblock_logical,
				   (unsigned long long)block);
		return -EUCLEAN;
	}
	return 0;
}

static int beamfs_inline_lookup_phys(struct inode *inode, u64 iblock_logical,
				     u64 *phys_out)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	struct buffer_head *ibh = NULL;
	__le64                   *ptrs;
	u64                       indirect_blk;
	u64                       indirect_slot;
	u64                       phys;
	int                       ret;

	if (!phys_out)
		return -EINVAL;

	*phys_out = 0;

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS) {
		u64 dphys = le64_to_cpu(fi->i_direct[iblock_logical]);

		if (dphys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (dphys < sbi->s_data_start ||
			    dphys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted direct pointer ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)dphys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			if (!beamfs_block_is_allocated(sb, phys)) {
				pr_err_ratelimited("beamfs/inline: unallocated direct pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys);
				return -EUCLEAN;
			}
		}
		*phys_out = dphys;
		return 0;
	}

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS) {
		indirect_slot = iblock_logical - BEAMFS_DIRECT_BLOCKS;
		indirect_blk  = le64_to_cpu(fi->i_indirect);

		if (!indirect_blk)
			return 0; /* HOLE: indirect block not yet allocated */

		ret = beamfs_check_intermediate_block(sb, indirect_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "indirect");
		if (ret)
			return ret;

		/* Already held when this call created it. */
		{
			int fresh = 0;

			if (!ibh) {
				/*
				 * find_get_block, not sb_getblk: the
				 * question is whether the cache already
				 * holds this block, and sb_getblk would
				 * answer it by creating one. A probe that
				 * changes what it measures is worse than
				 * no probe -- generic/464 passed with one
				 * and the result could not be read.
				 */
				struct buffer_head *probe =
					sb_find_get_block(sb, indirect_blk);

				fresh = !probe;
				if (probe)
					brelse(probe);
				ibh = sb_bread(sb, indirect_blk);
			}
			if (!ibh) {
				pr_err_ratelimited("beamfs/inline: failed to read indirect block %llu\n",
						   (unsigned long long)indirect_blk);
				return -EIO;
			}
			trace_beamfs_ind_read(inode->i_ino, indirect_blk,
				(unsigned int)indirect_slot,
				le64_to_cpu(((__le64 *)ibh->b_data)[indirect_slot]),
				buffer_uptodate(ibh), fresh);
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		phys = le64_to_cpu(ptrs[indirect_slot]);
		brelse(ibh);

		if (phys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (phys < sbi->s_data_start ||
			    phys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted indirect pointer ino=%llu iblock=%llu slot=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)indirect_slot,
						   (unsigned long long)phys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			if (!beamfs_block_is_allocated(sb, phys)) {
				pr_err_ratelimited("beamfs/inline: unallocated indirect pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys);
				return -EUCLEAN;
			}
		}
		*phys_out = phys;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_DINDIRECT) {
		/*
		 * Double-indirect lookup. Map iblock to (level-1 slot,
		 * level-2 slot) where level-1 is an indirect block of
		 * pointers to indirect blocks, and level-2 is the leaf
		 * indirect block whose entries point to user data blocks.
		 * Coverage: 12 + 512 + 512*512 = 262668 iblocks ~= 1 GiB.
		 */
		u64 didx, l1_slot, l2_slot, dindirect_blk, l1_blk;
		struct buffer_head *l1bh = NULL;

		didx = iblock_logical - BEAMFS_MAX_IBLOCK_INDIRECT;
		l1_slot = didx / BEAMFS_INDIRECT_PTRS;
		l2_slot = didx % BEAMFS_INDIRECT_PTRS;

		dindirect_blk = le64_to_cpu(fi->i_dindirect);
		if (!dindirect_blk)
			return 0; /* HOLE: dindirect block not yet allocated */

		ret = beamfs_check_intermediate_block(sb, dindirect_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "dindirect");
		if (ret)
			return ret;

		ibh = sb_bread(sb, dindirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read dindirect block %llu\n",
					   (unsigned long long)dindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		brelse(ibh);

		if (!l1_blk)
			return 0; /* HOLE: level-1 indirect not allocated */

		ret = beamfs_check_intermediate_block(sb, l1_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "dindirect L1");
		if (ret)
			return ret;

		/* Already held when this call created it. */
		if (!l1bh)
			l1bh = sb_bread(sb, l1_blk);
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read dindirect L1 block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		phys = le64_to_cpu(ptrs[l2_slot]);
		brelse(l1bh);

		if (phys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (phys < sbi->s_data_start ||
			    phys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted dindirect pointer ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			if (!beamfs_block_is_allocated(sb, phys)) {
				pr_err_ratelimited("beamfs/inline: unallocated dindirect pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys);
				return -EUCLEAN;
			}
		}
		*phys_out = phys;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_TINDIRECT) {
		/*
		 * Triple-indirect lookup. Map iblock to (level-1, level-2,
		 * level-3) where level-1 is an indirect block of pointers to
		 * dindirect blocks, level-2 is an indirect block of pointers
		 * to indirect blocks, and level-3 is the leaf indirect block
		 * whose entries point to user data blocks.
		 * Coverage: 12 + 512 + 512^2 + 512^3 = 134480396 iblocks
		 * ~= 478 GiB at 3824 user bytes/block.
		 */
		u64 tidx, l1_slot, l2_slot, l3_slot;
		u64 tindirect_blk, l1_blk, l2_blk;
		struct buffer_head *l1bh = NULL, *l2bh = NULL;

		tidx = iblock_logical - BEAMFS_MAX_IBLOCK_DINDIRECT;
		l1_slot = tidx / (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS);
		l2_slot = (tidx / BEAMFS_INDIRECT_PTRS) % BEAMFS_INDIRECT_PTRS;
		l3_slot = tidx % BEAMFS_INDIRECT_PTRS;

		tindirect_blk = le64_to_cpu(fi->i_tindirect);
		if (!tindirect_blk)
			return 0; /* HOLE: tindirect block not yet allocated */

		ret = beamfs_check_intermediate_block(sb, tindirect_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "tindirect");
		if (ret)
			return ret;

		ibh = sb_bread(sb, tindirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect block %llu\n",
					   (unsigned long long)tindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		brelse(ibh);

		if (!l1_blk)
			return 0; /* HOLE: level-1 not allocated */

		ret = beamfs_check_intermediate_block(sb, l1_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "tindirect L1");
		if (ret)
			return ret;

		/* Already held when this call created it. */
		if (!l1bh)
			l1bh = sb_bread(sb, l1_blk);
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L1 block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		l2_blk = le64_to_cpu(ptrs[l2_slot]);
		brelse(l1bh);

		if (!l2_blk)
			return 0; /* HOLE: level-2 not allocated */

		ret = beamfs_check_intermediate_block(sb, l2_blk,
						      (unsigned long long)inode->i_ino,
						      iblock_logical,
						      "tindirect L2");
		if (ret)
			return ret;

		/* Already held when this call created it. */
		if (!l2bh)
			l2bh = sb_bread(sb, l2_blk);
		if (!l2bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L2 block %llu\n",
					   (unsigned long long)l2_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, l2bh)) {
			brelse(l2bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l2bh->b_data;
		phys = le64_to_cpu(ptrs[l3_slot]);
		brelse(l2bh);

		if (phys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (phys < sbi->s_data_start ||
			    phys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted tindirect pointer ino=%llu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
			if (!beamfs_block_is_allocated(sb, phys)) {
				pr_err_ratelimited("beamfs/inline: unallocated tindirect pointer ino=%llu iblock=%llu phys=%llu\n",
						   (unsigned long long)inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)phys);
				return -EUCLEAN;
			}
		}
		*phys_out = phys;
		return 0;
	}

	pr_err_ratelimited("beamfs/inline: iblock %llu beyond tindirect capacity\n",
			   (unsigned long long)iblock_logical);
	return -EOPNOTSUPP;
}

/* ------------------------------------------------------------------------- */
/* Allocating block-mapping (v2 INLINE write path)                           */
/*                                                                           */
/* Variant of beamfs_inline_lookup_phys() that allocates on demand. Used by  */
/* the write path (write_begin + writepages) to map a logical iblock to a    */
/* physical block, allocating direct/indirect/data blocks as needed and      */
/* zero-initializing freshly allocated data blocks so a subsequent read sees */
/* deterministic content (16 zero subblocks of valid RS codewords -- the    */
/* zero data plus zero parity is a valid RS(255,239) codeword by linearity). */
/*                                                                           */
/* Allocates:                                                                */
/*   - Direct: writes phys into fi->i_direct[iblock] + mark_inode_dirty.     */
/*   - Indirect: allocates the indirect block first (zero-init) if absent,   */
/*     then allocates the data block and writes phys into ptrs[slot].        */
/*                                                                           */
/* The freshly allocated data block is zero-initialized via sb_getblk +      */
/* memset, NOT via sb_bread, because there is no on-disk content to read     */
/* (the block was unallocated). The buffer is marked uptodate + dirty so     */
/* writepages can RMW it without an extra sb_bread.                          */
/*                                                                           */
/* Returns:                                                                  */
/*   0  + *phys_out = block number (mapped or freshly allocated)             */
/*   <0 on error (-ENOSPC if alloc fails, -EIO on indirect read fail,        */
/*                -EOPNOTSUPP beyond v1 capacity)                            */
/* ------------------------------------------------------------------------- */
/*
 * Is this a block a file may point at?
 *
 * Everything below s_data_start is metadata: the superblock, the inode
 * table, the bitmap, the indirect parity region. A file's data never
 * lives there, and a pointer naming it is wrong whatever produced it.
 *
 * generic/075 found two blocks of the inode table holding fsx's fill
 * bytes, seven inodes in them beyond correction and 40437 pointers
 * naming blocks past the end of the device -- and the first thing to
 * notice was fsck, three minutes and one unmount later, by which time
 * the path that did it was gone.
 *
 * Refused here instead, with the block and the inode named, while the
 * stack that produced it is still on the stack.
 */
static bool beamfs_phys_is_sane(struct super_block *sb, struct inode *inode,
				u64 phys, const char *where)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u64 last = (u64)sbi->s_data_start + sbi->s_nblocks;

	if (phys == 0)
		return true;			/* a hole, not an address */

	if (phys < sbi->s_data_start || phys >= last) {
		/*
		 * The inode number in its own variable: a ternary between
		 * i_ino and a literal is promoted, and %lu given the
		 * result compiles clean where the promotion happens to
		 * land on unsigned long and breaks where it does not.
		 */
		unsigned long ino = inode ? inode->i_ino : 0;

		pr_err_ratelimited("beamfs: %s: inode %lu names block %llu, outside %llu..%llu\n",
				   where, ino,
				   (unsigned long long)phys,
				   (unsigned long long)sbi->s_data_start,
				   (unsigned long long)last - 1);
		beamfs_fail(sb, where, -EUCLEAN);
		return false;
	}
	return true;
}

#ifdef CONFIG_BEAMFS_ORDERED_META
/*
 * Put a metadata block on the medium before anything names it.
 *
 * The safe order for a tree is pointee before pointer, always, and
 * nothing in beamfs establishes it: mark_buffer_dirty hands the buffer
 * to writeback, which sorts by age and by position.
 *
 * This is the brute form -- one synchronous write per level, up to
 * four for a block reached through triple indirection -- and it is
 * meant to answer one question: is the missing order the reason nine
 * xfstests fail? A journal is how a filesystem gets the same property
 * without paying this.
 */
static void beamfs_order_before_pointer(struct buffer_head *bh)
{
	if (!bh)
		return;
	/*
	 * write_dirty_buffer submits and returns; the wait is what makes
	 * this an ordering and not a hint.
	 */
	write_dirty_buffer(bh, REQ_SYNC);
	wait_on_buffer(bh);
}
#else
static inline void beamfs_order_before_pointer(struct buffer_head *bh)
{
	(void)bh;
}
#endif

/*
 * Did this call allocate, or did it find what was already there?
 *
 * iomap needs the answer. A write shorter than the mapping it was
 * given leaves the rest of that mapping allocated, and only the blocks
 * this call created may be given back -- a block that was already
 * there belongs to the file whether or not this write reached it.
 *
 * generic/013 leaves 28 consecutive blocks allocated and never
 * written, the length of one write, and nothing can tell them from
 * blocks the file already owned.
 *
 * @allocated may be NULL for callers that do not care.
 */
static int beamfs_inline_lookup_or_alloc_phys_new(struct inode *inode,
						  u64 iblock_logical,
						  u64 *phys_out,
						  bool *allocated);

static int beamfs_inline_lookup_or_alloc_phys(struct inode *inode,
					      u64 iblock_logical,
					      u64 *phys_out)
{
	return beamfs_inline_lookup_or_alloc_phys_new(inode, iblock_logical,
						      phys_out, NULL);
}

/*
 * Seal a block: encode it in whichever layout the volume uses.
 *
 * Four sites encode a freshly allocated data block with the same six
 * arguments, and a fifth and sixth do it for writeback. One function
 * so a layout change touches one place -- the four were identical
 * character for character, which is how a fifth comes to differ by
 * accident.
 */
/*
 * Lay a block's payload down in whichever layout the volume uses.
 *
 * Alternating, the payload is sixteen runs of 239 bytes 255 apart, and
 * the copy has to break it up. A capsule holds it contiguously, and
 * breaking it up writes every run 16 bytes further along than it
 * belongs -- measured as the last 222 bytes of every block coming back
 * wrong, which is where the fifteenth run ends up.
 *
 * Paired with beamfs_seal_block, which encodes what this writes.
 */
static void beamfs_lay_payload(struct super_block *sb, u8 *block,
			       const u8 *payload)
{
	unsigned int i;

	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) {
		memcpy(block + BEAMFS_CAPSULE_DATA_OFF, payload,
		       BEAMFS_CAPSULE_DATA_BYTES);
		return;
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy(block + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       payload + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
}

static int beamfs_seal_block(struct super_block *sb, u8 *block)
{
	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE)
		return beamfs_rs_encode_woven(block + BEAMFS_CAPSULE_DATA_OFF,
					      block + BEAMFS_CAPSULE_PARITY_OFF,
					      BEAMFS_RS_PARITY,
					      BEAMFS_SUBBLOCK_DATA,
					      BEAMFS_DATA_INLINE_SUBBLOCKS);

	return beamfs_rs_encode_region(block, BEAMFS_SUBBLOCK_TOTAL,
				       block + BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_SUBBLOCK_TOTAL,
				       BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_DATA_INLINE_SUBBLOCKS);
}

static int beamfs_inline_lookup_or_alloc_phys_new(struct inode *inode,
						  u64 iblock_logical,
						  u64 *phys_out,
						  bool *allocated)
{
	/*
	 * False until proven otherwise: a caller reading this after an
	 * error path must not find a stale true.
	 */
	if (allocated)
		*allocated = false;

	/*
	 * Nothing free: say so once and return, rather than walking the
	 * whole indirection tree to discover it at every level.
	 *
	 * Without this a full volume produced one failed allocation per
	 * block per level -- 19531 suppressed log callbacks in three
	 * seconds under generic/224 -- and the writeback path spent its
	 * time in the allocator and the ring buffer instead of returning
	 * ENOSPC. The test then exceeded its timeout for want of an error
	 * the filesystem already knew.
	 *
	 * s_free_blocks is read without the lock. It is a hint here: a
	 * concurrent free racing this check costs one retry, and the
	 * allocation below is still the thing that decides.
	 */
	/*
	 * Nothing to allocate for an inode being destroyed.
	 *
	 * __mark_inode_dirty returns without queueing when I_FREEING is
	 * set -- fs-writeback.c, "if (inode_state_read(inode) &
	 * I_FREEING) goto out_unlock". A block allocated here has its
	 * pointer installed into a tree whose inode will never be
	 * written: the bitmap says used, nothing references it, and fsck
	 * calls it used-but-unreferenced.
	 *
	 * The check belongs here rather than in the callers, because all
	 * three of them -- iomap_begin, writeback_range, zero_tail_block
	 * -- can run against an evicting inode, and guarding one of the
	 * three leaves the other two leaking.
	 *
	 * ext2 cannot reach this: ext2_get_block runs with create=0 on
	 * the writeback path, and an evicting inode has been through
	 * truncate_inode_pages_final. beamfs allocates at writeback and
	 * has to check for itself.
	 */
	if (inode_state_read_once(inode) &
	    (I_FREEING | I_WILL_FREE | I_CLEAR))
		return -EIO;

	if (BEAMFS_SB(inode->i_sb)->s_free_blocks == 0)
		return -ENOSPC;

	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	struct buffer_head *ibh = NULL;
	struct buffer_head       *dbh;
	__le64                   *ptrs;
	u64                       indirect_blk;
	u64                       indirect_slot;
	u64                       phys;
	u64                       new_block;

	if (!phys_out)
		return -EINVAL;

	*phys_out = 0;

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS) {
		/* --- Direct block --- */
		phys = le64_to_cpu(fi->i_direct[iblock_logical]);
		if (phys) {
			*phys_out = phys;
			return 0;
		}
		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			pr_warn_once("beamfs/inline: volume full, first refusal at direct\n");
			return -ENOSPC;
		}
		/* Zero-init the freshly allocated data block on disk. */
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback_folio and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);
		brelse(dbh);

		/*
		 * The direct array is a slot store like any other.
		 *
		 * The three indirect levels emit this and the direct array
		 * did not, so a probe pairing allocations against stores
		 * counted every direct block as an allocation that went
		 * nowhere: 176180 of them in one generic/269, against 327
		 * blocks fsck actually finds leaked.
		 *
		 * parent 0 and level 0: there is no block above a direct
		 * pointer, and nothing else uses those values.
		 */
		trace_beamfs_slot_store(inode->i_ino, 0, iblock_logical,
					le64_to_cpu(fi->i_direct[iblock_logical]),
					new_block, 0);

		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		fi->i_direct[iblock_logical] = cpu_to_le64(new_block);
		mark_inode_dirty(inode);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS) {
		/* --- Single indirect block --- */
		indirect_slot = iblock_logical - BEAMFS_DIRECT_BLOCKS;
		indirect_blk  = le64_to_cpu(fi->i_indirect);

		if (!indirect_blk) {
			/* Allocate indirect block first, zero-init. */
			indirect_blk = beamfs_alloc_block(sb, inode);
			if (!indirect_blk) {
				pr_warn_once("beamfs/inline: volume full, first refusal at indirect\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, indirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, indirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			beamfs_tc_zeroed(sb, inode->i_ino, ibh->b_blocknr, __func__);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			/*
			 * Parity taken under the lock, like the install
			 * sites do.
			 *
			 * The block is allocated and visible before its
			 * pointer is installed a few lines down, so
			 * another writer can reach it. Computing parity
			 * after unlock_buffer describes whatever the block
			 * held at that instant, which need not be what
			 * reaches the medium: generic/464 writes and
			 * fsyncs the same files from several tasks and
			 * produced 194 indirect blocks whose parity was
			 * written and did not match, seven orphaned blocks
			 * under each.
			 */
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(ibh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */

		/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(ibh);
			fi->i_indirect = cpu_to_le64(indirect_blk);
			mark_inode_dirty(inode);
		}

		/* Read indirect to look up / install the slot. */
		/* Already held when this call created it. */
		if (!ibh)
			ibh = sb_bread(sb, indirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read indirect block %llu\n",
					  (unsigned long long)indirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		phys = le64_to_cpu(ptrs[indirect_slot]);

		if (phys) {
			brelse(ibh);
			*phys_out = phys;
			return 0;
		}

		/* Allocate data block and zero-init. */
		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			brelse(ibh);
			pr_warn_once("beamfs/inline: volume full, first refusal at data\n");
			return -ENOSPC;
		}
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			brelse(ibh);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback_folio and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);
		brelse(dbh);

		/*
		 * Under the buffer lock, like ext2 splicing a branch.
		 *
		 * This stores a pointer into a shared indirect block. Without
		 * the lock the store races the flusher submitting that same
		 * buffer: the pointer reaches disk half-written, or a
		 * concurrent installer's store is lost. generic/464 lost
		 * blocks whose final event was exactly this store into an L1
		 * shared across inodes, with no free and no truncate after.
		 * ind_parity_update reads b_data, so it belongs inside the
		 * lock too.
		 */
		lock_buffer(ibh);
		trace_beamfs_slot_store(inode->i_ino, ibh->b_blocknr,
					indirect_slot, le64_to_cpu(ptrs[indirect_slot]),
					new_block, 1);
		beamfs_tc_store(sb, inode->i_ino, ibh->b_blocknr,
				(u32)indirect_slot, le64_to_cpu(ptrs[indirect_slot]), new_block,
			__func__);
		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		ptrs[indirect_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, ibh, inode);
		unlock_buffer(ibh);
		/*
		 * Attach the block to the inode and dirty the inode, the
		 * way ext2_splice_branch ends:
		 *
		 *   mmb_mark_buffer_dirty(where->bh,
		 *                         &EXT2_I(inode)->i_metadata_bhs);
		 *   inode_set_ctime_current(inode);
		 *   mark_inode_dirty(inode);
		 *
		 * A plain mark_buffer_dirty leaves the block on no inode's
		 * metadata list, so __writeback_single_inode never flushes
		 * it: the pointer is in memory, the buffer is dirty, and
		 * nothing carries it to the disk. The three sites that
		 * create a fresh indirect block already dirty the inode --
		 * they change i_indirect -- and these three, installing
		 * into a block that already exists, did not.
		 *
		 * That is the generic/464 leak: 28 blocks allocated by one
		 * writer into one inode from this site, no free after, and
		 * the flusher reallocating for the same inode seconds later
		 * because the tree it read had none of them.
		 */
		mmb_mark_buffer_dirty(ibh, &BEAMFS_I(inode)->i_metadata_bhs);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		brelse(ibh);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_DINDIRECT) {
		/*
		 * Double-indirect write path: allocate dindirect block,
		 * level-1 indirect block, and data block in a cascade,
		 * each zero-initialized. The allocation order minimizes
		 * orphaned blocks on crash: data block last, so a crash
		 * between dindirect alloc and data alloc leaves a zero
		 * level-1 pointer (treated as HOLE on read).
		 */
		u64 didx, l1_slot, l2_slot;
		u64 dindirect_blk, l1_blk;
		struct buffer_head *l1bh = NULL;

		didx = iblock_logical - BEAMFS_MAX_IBLOCK_INDIRECT;
		l1_slot = didx / BEAMFS_INDIRECT_PTRS;
		l2_slot = didx % BEAMFS_INDIRECT_PTRS;

		/* --- Stage 1: dindirect block --- */
		dindirect_blk = le64_to_cpu(fi->i_dindirect);
		if (!dindirect_blk) {
			dindirect_blk = beamfs_alloc_block(sb, inode);
			if (!dindirect_blk) {
				pr_warn_once("beamfs/inline: volume full, first refusal at dindirect\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, dindirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, dindirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			beamfs_tc_zeroed(sb, inode->i_ino, ibh->b_blocknr, __func__);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			/*
			 * Parity taken under the lock, like the install
			 * sites do.
			 *
			 * The block is allocated and visible before its
			 * pointer is installed a few lines down, so
			 * another writer can reach it. Computing parity
			 * after unlock_buffer describes whatever the block
			 * held at that instant, which need not be what
			 * reaches the medium: generic/464 writes and
			 * fsyncs the same files from several tasks and
			 * produced 194 indirect blocks whose parity was
			 * written and did not match, seven orphaned blocks
			 * under each.
			 */
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(ibh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
		/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(ibh);
			fi->i_dindirect = cpu_to_le64(dindirect_blk);
			mark_inode_dirty(inode);
		}

		/* --- Stage 2: level-1 indirect block --- */
		ibh = sb_bread(sb, dindirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read dindirect block %llu\n",
					   (unsigned long long)dindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		if (!l1_blk) {
			l1_blk = beamfs_alloc_block(sb, inode);
			if (!l1_blk) {
				brelse(ibh);
				pr_warn_once("beamfs/inline: volume full, first refusal at L1 indirect\n");
				return -ENOSPC;
			}
			l1bh = sb_getblk(sb, l1_blk);
			if (!l1bh) {
				beamfs_free_block(sb, l1_blk, inode);
				brelse(ibh);
				return -EIO;
			}
			lock_buffer(l1bh);
			beamfs_tc_zeroed(sb, inode->i_ino, l1bh->b_blocknr, __func__);
			memset(l1bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l1bh);
			unlock_buffer(l1bh);
			beamfs_ind_parity_update(sb, l1bh, inode);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(l1bh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			beamfs_tc_store(sb, inode->i_ino, ibh->b_blocknr,
					(u32)l1_slot, le64_to_cpu(ptrs[l1_slot]), l1_blk,
				__func__);
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(l1bh);
			ptrs[l1_slot] = cpu_to_le64(l1_blk);
			beamfs_ind_parity_update(sb, ibh, inode);
			/* Splicing a child into its parent is an install like
			 * any other: the parent goes on the inode's list too,
			 * or it is dirty on nobody's.
			 */
			mmb_mark_buffer_dirty(ibh,
					      &BEAMFS_I(inode)->i_metadata_bhs);
		}
		brelse(ibh);

		/* --- Stage 3: data block --- */
		/* Already held when this call created it. */
		if (!l1bh)
			l1bh = sb_bread(sb, l1_blk);
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read L1 indirect block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		phys = le64_to_cpu(ptrs[l2_slot]);
		if (phys) {
			brelse(l1bh);
			*phys_out = phys;
			return 0;
		}

		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			brelse(l1bh);
			pr_warn_once("beamfs/inline: volume full, first refusal at dindirect data\n");
			return -ENOSPC;
		}
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			brelse(l1bh);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback_folio and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);
		brelse(dbh);

		/*
		 * Under the buffer lock, like ext2 splicing a branch.
		 *
		 * This stores a pointer into a shared indirect block. Without
		 * the lock the store races the flusher submitting that same
		 * buffer: the pointer reaches disk half-written, or a
		 * concurrent installer's store is lost. generic/464 lost
		 * blocks whose final event was exactly this store into an L1
		 * shared across inodes, with no free and no truncate after.
		 * ind_parity_update reads b_data, so it belongs inside the
		 * lock too.
		 */
		lock_buffer(l1bh);
		trace_beamfs_slot_store(inode->i_ino, l1bh->b_blocknr,
					l2_slot, le64_to_cpu(ptrs[l2_slot]),
					new_block, 2);
		beamfs_tc_store(sb, inode->i_ino, l1bh->b_blocknr,
				(u32)l2_slot, le64_to_cpu(ptrs[l2_slot]), new_block,
			__func__);
		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		ptrs[l2_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, l1bh, inode);
		unlock_buffer(l1bh);
		/*
		 * Attach the block to the inode and dirty the inode, the
		 * way ext2_splice_branch ends:
		 *
		 *   mmb_mark_buffer_dirty(where->bh,
		 *                         &EXT2_I(inode)->i_metadata_bhs);
		 *   inode_set_ctime_current(inode);
		 *   mark_inode_dirty(inode);
		 *
		 * A plain mark_buffer_dirty leaves the block on no inode's
		 * metadata list, so __writeback_single_inode never flushes
		 * it: the pointer is in memory, the buffer is dirty, and
		 * nothing carries it to the disk. The three sites that
		 * create a fresh indirect block already dirty the inode --
		 * they change i_indirect -- and these three, installing
		 * into a block that already exists, did not.
		 *
		 * That is the generic/464 leak: 28 blocks allocated by one
		 * writer into one inode from this site, no free after, and
		 * the flusher reallocating for the same inode seconds later
		 * because the tree it read had none of them.
		 */
		mmb_mark_buffer_dirty(l1bh, &BEAMFS_I(inode)->i_metadata_bhs);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		brelse(l1bh);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	if (iblock_logical < BEAMFS_MAX_IBLOCK_TINDIRECT) {
		/*
		 * Triple-indirect write path: allocate tindirect block,
		 * level-1 indirect block, level-2 indirect block, and data
		 * block in a cascade, each zero-initialized. Allocation order
		 * minimizes orphaned blocks on crash: data block last.
		 */
		u64 tidx, l1_slot, l2_slot, l3_slot;
		u64 tindirect_blk, l1_blk, l2_blk;
		struct buffer_head *l1bh = NULL, *l2bh = NULL;

		tidx = iblock_logical - BEAMFS_MAX_IBLOCK_DINDIRECT;
		l1_slot = tidx / (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS);
		l2_slot = (tidx / BEAMFS_INDIRECT_PTRS) % BEAMFS_INDIRECT_PTRS;
		l3_slot = tidx % BEAMFS_INDIRECT_PTRS;

		/* --- Stage 1: tindirect block --- */
		tindirect_blk = le64_to_cpu(fi->i_tindirect);
		if (!tindirect_blk) {
			tindirect_blk = beamfs_alloc_block(sb, inode);
			if (!tindirect_blk) {
				pr_warn_once("beamfs/inline: volume full, first refusal at tindirect\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, tindirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, tindirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			beamfs_tc_zeroed(sb, inode->i_ino, ibh->b_blocknr, __func__);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			/*
			 * Parity taken under the lock, like the install
			 * sites do.
			 *
			 * The block is allocated and visible before its
			 * pointer is installed a few lines down, so
			 * another writer can reach it. Computing parity
			 * after unlock_buffer describes whatever the block
			 * held at that instant, which need not be what
			 * reaches the medium: generic/464 writes and
			 * fsyncs the same files from several tasks and
			 * produced 194 indirect blocks whose parity was
			 * written and did not match, seven orphaned blocks
			 * under each.
			 */
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(ibh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
		/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(ibh);
			fi->i_tindirect = cpu_to_le64(tindirect_blk);
			mark_inode_dirty(inode);
		}

		/* --- Stage 2: level-1 indirect block --- */
		ibh = sb_bread(sb, tindirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect block %llu\n",
					   (unsigned long long)tindirect_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)ibh->b_data;
		l1_blk = le64_to_cpu(ptrs[l1_slot]);
		if (!l1_blk) {
			l1_blk = beamfs_alloc_block(sb, inode);
			if (!l1_blk) {
				brelse(ibh);
				pr_warn_once("beamfs/inline: volume full, first refusal at tindirect L1\n");
				return -ENOSPC;
			}
			l1bh = sb_getblk(sb, l1_blk);
			if (!l1bh) {
				beamfs_free_block(sb, l1_blk, inode);
				brelse(ibh);
				return -EIO;
			}
			lock_buffer(l1bh);
			beamfs_tc_zeroed(sb, inode->i_ino, l1bh->b_blocknr, __func__);
			memset(l1bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l1bh);
			unlock_buffer(l1bh);
			beamfs_ind_parity_update(sb, l1bh, inode);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(l1bh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			beamfs_tc_store(sb, inode->i_ino, ibh->b_blocknr,
					(u32)l1_slot, le64_to_cpu(ptrs[l1_slot]), l1_blk,
				__func__);
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(l1bh);
			ptrs[l1_slot] = cpu_to_le64(l1_blk);
			beamfs_ind_parity_update(sb, ibh, inode);
			/* Splicing a child into its parent is an install like
			 * any other: the parent goes on the inode's list too,
			 * or it is dirty on nobody's.
			 */
			mmb_mark_buffer_dirty(ibh,
					      &BEAMFS_I(inode)->i_metadata_bhs);
		}
		brelse(ibh);

		/* --- Stage 3: level-2 indirect block --- */
		/* Already held when this call created it. */
		if (!l1bh)
			l1bh = sb_bread(sb, l1_blk);
		if (!l1bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L1 block %llu\n",
					   (unsigned long long)l1_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, l1bh)) {
			brelse(l1bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l1bh->b_data;
		l2_blk = le64_to_cpu(ptrs[l2_slot]);
		if (!l2_blk) {
			l2_blk = beamfs_alloc_block(sb, inode);
			if (!l2_blk) {
				brelse(l1bh);
				pr_warn_once("beamfs/inline: volume full, first refusal at tindirect L2\n");
				return -ENOSPC;
			}
			l2bh = sb_getblk(sb, l2_blk);
			if (!l2bh) {
				beamfs_free_block(sb, l2_blk, inode);
				brelse(l1bh);
				return -EIO;
			}
			lock_buffer(l2bh);
			beamfs_tc_zeroed(sb, inode->i_ino, l2bh->b_blocknr, __func__);
			memset(l2bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l2bh);
			unlock_buffer(l2bh);
			beamfs_ind_parity_update(sb, l2bh, inode);
			/*
			 * Attach it to the inode as the install sites do.
			 *
			 * A fresh indirect block is dirtied here and its first
			 * pointer installed a few lines further down, and the
			 * install attaches it. But sb_getblk here and sb_bread
			 * there need not hand back the same buffer: if this one
			 * is evicted in between, it goes with nothing written,
			 * on no inode's list for anyone to flush.
			 *
			 * generic/464 loses exactly that. Inode 150 in one
			 * capture: four pointers installed into indirect block
			 * 33190, i_indirect written to disk naming it, and 33190
			 * itself still 0xcd -- never written at all -- so fsck
			 * finds four blocks marked used that nothing references.
			 */
			mmb_mark_buffer_dirty(l2bh, &BEAMFS_I(inode)->i_metadata_bhs);
			/*
			 * Held, not released and read back.
			 *
			 * This released the buffer here and read the same block
			 * again a few lines down. Between the two it can be
			 * evicted, and the read then comes from the medium --
			 * where nothing has been written yet. The block returns
			 * holding whatever was there before, and every pointer
			 * installed into it afterwards lands in a block that is
			 * never written at all.
			 *
			 * A frozen generic/464 volume shows the whole thing:
			 * inode 36's indirect block 16469, named by the inode on
			 * disk, 0xcd across 464 of its 512 slots, and 273 blocks
			 * reported used and referenced by nothing.
			 *
			 * Attaching it to the inode was meant to close this. It
			 * does not: the attach makes the buffer flushable, and
			 * dropping the last reference still lets it go before
			 * anything flushes it. Holding the reference closes it.
			 */
			beamfs_tc_store(sb, inode->i_ino, l1bh->b_blocknr,
					(u32)l2_slot, le64_to_cpu(ptrs[l2_slot]), l2_blk,
				__func__);
			/* The block, before the pointer that names it. */
			beamfs_order_before_pointer(l2bh);
			ptrs[l2_slot] = cpu_to_le64(l2_blk);
			beamfs_ind_parity_update(sb, l1bh, inode);
			/* Splicing a child into its parent is an install like
			 * any other: the parent goes on the inode's list too,
			 * or it is dirty on nobody's.
			 */
			mmb_mark_buffer_dirty(l1bh,
					      &BEAMFS_I(inode)->i_metadata_bhs);
		}
		brelse(l1bh);

		/* --- Stage 4: data block --- */
		/* Already held when this call created it. */
		if (!l2bh)
			l2bh = sb_bread(sb, l2_blk);
		if (!l2bh) {
			pr_err_ratelimited("beamfs/inline: failed to read tindirect L2 block %llu\n",
					   (unsigned long long)l2_blk);
			return -EIO;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, l2bh)) {
			brelse(l2bh);
			return -EUCLEAN;
		}
		ptrs = (__le64 *)l2bh->b_data;
		phys = le64_to_cpu(ptrs[l3_slot]);
		if (phys) {
			brelse(l2bh);
			*phys_out = phys;
			return 0;
		}

		new_block = beamfs_alloc_block(sb, inode);
		if (!new_block) {
			brelse(l2bh);
			pr_warn_once("beamfs/inline: volume full, first refusal at tindirect data\n");
			return -ENOSPC;
		}
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block, inode);
			brelse(l2bh);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		/*
		 * Stamp the DATA_CSUM descriptor on the freshly allocated data
		 * block. An all-zero block is a valid RS codeword by linearity,
		 * so decode accepts it, but the descriptor would stay 0x00 and
		 * the read path would have nothing to verify against. Before
		 * the type byte was made fail-closed that meant silent
		 * acceptance; afterwards it means a legitimate newly allocated
		 * block is rejected. Observed 2026-08-18: iblocks 0..5 of a
		 * test file logged "bad descriptor type=0x00" on five runs out
		 * of six, because allocation writes the block here while the
		 * only other stamp sites are writeback_folio and
		 * zero_tail_block, neither of which runs for a block that is
		 * allocated but not yet written through the folio path. The
		 * payload is the zeroed block itself. Indirect blocks carry no
		 * DATA_CSUM descriptor and are deliberately left untouched.
		 */
		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)dbh->b_data,
					     (const u8 *)dbh->b_data,
					     (unsigned long long)inode->i_ino, iblock_logical);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		/*
		 * A data block carries sixteen RS codewords, not indirect
		 * parity.
		 *
		 * ind_parity_update files a CRC for this block number in
		 * the separate parity region that covers indirect blocks.
		 * A newly allocated data block was being registered there,
		 * and its own sixteen codewords were left as the zeros
		 * memset put down -- correct for a zero block, wrong the
		 * moment anything is written into it and the parity is not
		 * recomputed.
		 *
		 * The sweep found exactly that: subblocks holding data,
		 * uncorrectable; subblocks still zero, fine. Encoding here
		 * makes a fresh block valid on its own terms from the
		 * start.
		 */
		{
			int _e = beamfs_seal_block(sb, (u8 *)dbh->b_data);

			if (_e < 0)
				pr_err_ratelimited("beamfs/inline: encode of new data block %llu failed: %d\n",
						   (unsigned long long)new_block,
						   _e);
		}
		mark_buffer_dirty(dbh);
		brelse(dbh);

		/*
		 * Under the buffer lock, like ext2 splicing a branch.
		 *
		 * This stores a pointer into a shared indirect block. Without
		 * the lock the store races the flusher submitting that same
		 * buffer: the pointer reaches disk half-written, or a
		 * concurrent installer's store is lost. generic/464 lost
		 * blocks whose final event was exactly this store into an L1
		 * shared across inodes, with no free and no truncate after.
		 * ind_parity_update reads b_data, so it belongs inside the
		 * lock too.
		 */
		lock_buffer(l2bh);
		trace_beamfs_slot_store(inode->i_ino, l2bh->b_blocknr,
					l3_slot, le64_to_cpu(ptrs[l3_slot]),
					new_block, 3);
		beamfs_tc_store(sb, inode->i_ino, l2bh->b_blocknr,
				(u32)l3_slot, le64_to_cpu(ptrs[l3_slot]), new_block,
			__func__);
		/* The block, before the pointer that names it. */
		beamfs_order_before_pointer(dbh);
		ptrs[l3_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, l2bh, inode);
		unlock_buffer(l2bh);
		/*
		 * Attach the block to the inode and dirty the inode, the
		 * way ext2_splice_branch ends:
		 *
		 *   mmb_mark_buffer_dirty(where->bh,
		 *                         &EXT2_I(inode)->i_metadata_bhs);
		 *   inode_set_ctime_current(inode);
		 *   mark_inode_dirty(inode);
		 *
		 * A plain mark_buffer_dirty leaves the block on no inode's
		 * metadata list, so __writeback_single_inode never flushes
		 * it: the pointer is in memory, the buffer is dirty, and
		 * nothing carries it to the disk. The three sites that
		 * create a fresh indirect block already dirty the inode --
		 * they change i_indirect -- and these three, installing
		 * into a block that already exists, did not.
		 *
		 * That is the generic/464 leak: 28 blocks allocated by one
		 * writer into one inode from this site, no free after, and
		 * the flusher reallocating for the same inode seconds later
		 * because the tree it read had none of them.
		 */
		mmb_mark_buffer_dirty(l2bh, &BEAMFS_I(inode)->i_metadata_bhs);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		brelse(l2bh);

		if (allocated)
			*allocated = true;
		*phys_out = new_block;
		return 0;
	}

	pr_err_ratelimited("beamfs/inline: iblock %llu beyond tindirect capacity (write)\n",
			   (unsigned long long)iblock_logical);
	return -EOPNOTSUPP;
}

/* ------------------------------------------------------------------------- */
/*
 * beamfs_inline_stamp_tail_pad -- write the DATA_CSUM descriptor into the
 * 16-byte block tail pad after RS encode. The pad is always fully zeroed
 * first, which covers the reserved bytes and the csum-disabled (NONE)
 * case. When the volume has DATA_CSUM active, stamp csum_type = CRC32 and
 * the beamfs_crc32 (crc32_le) of the 3824-byte decoded payload, the same
 * bytes just fed to the RS encoder. See format-v6.md sections 3 and 4.2.
 * The descriptor lives outside every RS codeword by design (3.3): a flip
 * in it yields at worst a false-positive fail-closed on read, never a
 * silent accept of wrong data.
 */
static void beamfs_inline_stamp_tail_pad(struct beamfs_sb_info *sbi,
					 u8 *block, const u8 *payload,
					 u64 ino, u64 iblock)
{
	memset(block + BEAMFS_DATA_INLINE_TOTAL, 0, BEAMFS_DATA_INLINE_PAD);
	if (sbi->s_data_csum) {
		u32 crc = beamfs_crc32(payload, BEAMFS_DATA_INLINE_BYTES);

		block[BEAMFS_DATA_CSUM_TYPE_OFF] = BEAMFS_CSUM_CRC32;
		put_unaligned_le32(crc, block + BEAMFS_DATA_CSUM_VALUE_OFF);
	}
	/*
	 * DATA_SELFID: bind the block to the (inode, logical index) pair it
	 * was written for, so a read reaching it through a corrupted pointer
	 * fails closed instead of returning another file's intact data.
	 */
	if (sbi->s_data_selfid) {
		u64 id = beamfs_data_selfid(ino, iblock);

		put_unaligned_le64(id, block + BEAMFS_DATA_SELFID_OFF);
	}
}

/*
 * beamfs_inline_payload_crc -- recompute the DATA_CSUM over the decoded
 * payload held in @codeword (post-RS-decode, interleaved 255-byte stride,
 * 16 subblocks of 239 data bytes). Chains crc32_le over the 16 data
 * segments; by associativity of crc32_le over concatenation this equals
 * beamfs_crc32() over the contiguous de-interleaved 3824-byte payload,
 * the value the write path stored (see beamfs_inline_stamp_tail_pad and
 * beamfs_crc32_sb for the same non-contiguous chaining idiom). No alloc.
 */
static u32 beamfs_inline_payload_crc(const u8 *codeword)
{
	u32 c = 0xFFFFFFFF;
	unsigned int i;

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		c = crc32_le(c,
			     codeword + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
			     BEAMFS_SUBBLOCK_DATA);
	return c ^ 0xFFFFFFFF;
}

/* beamfs_inline_decode_block_into_buf -- read disk block, RS-decode all 16  */
/*                                        subblocks, copy a user-byte slice  */
/*                                        into the supplied buffer.          */
/*                                                                           */
/* This helper is the algebraic decode operator Phi of Theorem v2.1: it      */
/* maps (phys, slice) -> user_bytes union {bottom}. Reused by single-block   */
/* read_folio (slice = full 0..3824) and the multi-block read/writeback      */
/* paths added in subsequent sub-steps (slice = portion of a disk block      */
/* covering one folio).                                                      */
/*                                                                           */
/* Inputs:                                                                   */
/*   sb                       superblock (used for sb_bread + journal)       */
/*   phys                     non-zero physical disk block number to decode  */
/*   inode                    inode (used for ino in pr_warn/journal)        */
/*   iblock_logical_for_log   logical iblock identifier for log lines        */
/*   dst_buf                  destination buffer; caller-allocated, must     */
/*                            have at least slice_length bytes available     */
/*   slice_offset             byte offset within the disk block user-area    */
/*                            (0..BEAMFS_DATA_INLINE_BYTES-1)                */
/*   slice_length             number of user bytes to copy into dst_buf      */
/*                            (1..BEAMFS_DATA_INLINE_BYTES)                  */
/*   rmw_path                 caller context flag: true when invoked from a  */
/*                            read-modify-write transit (writeback_folio or  */
/*                            zero_tail), false when invoked from a user-    */
/*                            initiated read (read_folio). Propagated into   */
/*                            the journal entry as                           */
/*                            BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED so that   */
/*                            silent neutralisation events become            */
/*                            empirically distinguishable from user-visible  */
/*                            corrections in post-run forensic analysis.     */
/*                            Also discriminates the pr_warn / pr_err log    */
/*                            line wording between "neutralised" and        */
/*                            "corrected" / "uncorrectable".                */
/*                                                                           */
/* Slice contract: slice_offset + slice_length <= BEAMFS_DATA_INLINE_BYTES   */
/* (3824). HOLE (phys == 0) is NOT handled here -- caller must check.        */
/*                                                                           */
/* On any subblock corrected: durable autonomic repair via mark_buffer_dirty */
/* + sync_dirty_buffer before return, same pattern as alloc.c bitmap path.   */
/* On any subblock uncorrectable: journal entry with UNCORRECTABLE flag,     */
/* then -EIO; no slice copy is performed.                                    */
/*                                                                           */
/* Anti-NAK rationale: this isolates the RS decode + autonomic repair logic  */
/* from folio lifecycle management. The folio lock and folio_end_read are    */
/* the caller's responsibility.                                              */
/* ------------------------------------------------------------------------- */
static int beamfs_inline_decode_block_into_buf(struct super_block *sb,
					       struct buffer_head *bh,
					       u64 phys,
					       struct inode *inode,
					       u64 iblock_logical_for_log,
					       u8 *dst_buf,
					       u32 slice_offset,
					       u32 slice_length,
					       bool rmw_path)
{
	u8                 *tmp = NULL;
	int                 rs_results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	int                 rs_positions[BEAMFS_DATA_INLINE_SUBBLOCKS *
				      (BEAMFS_RS_PARITY / 2)];
	bool                corrected = false;
	bool                uncorrectable = false;
	unsigned int        i;
	int                 ret = 0;
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

	/* Defensive contract checks (cheap; helpful in audit and fuzzing). */
	if (WARN_ON_ONCE(phys == 0))
		return -EINVAL;
	if (WARN_ON_ONCE(dst_buf == NULL))
		return -EINVAL;
	if (WARN_ON_ONCE(slice_length == 0))
		return -EINVAL;
	if (WARN_ON_ONCE((u64)slice_offset + slice_length >
			 BEAMFS_DATA_INLINE_BYTES))
		return -EINVAL;

	/*
	 * Contract: caller owns @bh -- it must be sb_bread'd and
	 * lock_buffer'd before calling, and unlock_buffer'd + brelse'd
	 * after. The lock_buffer serialises the RMW transit against
	 * other writers on the same physical block via the block-device
	 * page cache. Reads from bh->b_data into tmp happen here under
	 * that exclusion. See writeback_folio and zero_tail_block for
	 * the canonical callsite pattern.
	 *
	 * Decode RS(255,239) subblocks into a private scratch buffer
	 * (tmp), never into bh->b_data. decode_rs8 mutates both data
	 * and parity bytes in its input. The decode_rs8 upstream race
	 * on rs_control->buffers[] is resolved by the per-CPU
	 * rs_control allocation in edac.c.
	 */
	tmp = beamfs_scratch_get(sb);
	if (!tmp)
		return -ENOMEM;
	/*
	 * No lock_buffer here, and the reason is in the callers.
	 *
	 * Three of the five hold it already -- writeback_range,
	 * zero_tail_block and the RMW path all lock the buffer before
	 * decoding into it -- so taking it again deadlocks on the first
	 * decode, which on this filesystem means the rootfs mount. That
	 * was tried and the node never reached a login prompt.
	 *
	 * The two callers that do not hold it are read_folio_range and
	 * fiemap, where a torn read is possible and shows up as a
	 * spurious uncorrectable rather than as damage. Fixing that means
	 * locking in those two callers, not here.
	 */
	memcpy(tmp, bh->b_data, BEAMFS_BLOCK_SIZE);

	/*
	 * A capsule is gathered, not walked.
	 *
	 * Interleaved, symbol i of codeword j is at byte i*16 + j rather
	 * than j*239 + i, and the header sits inside the coded area. The
	 * two layouts share no arithmetic: reading one with the other's
	 * gathers the wrong symbols and decodes to noise with every
	 * check passing. The feature bit is what keeps them apart.
	 */
	if (BEAMFS_SB(sb)->s_feat_incompat &
	    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) {
		beamfs_rs_decode_woven(tmp + BEAMFS_CAPSULE_DATA_OFF,
				       tmp + BEAMFS_CAPSULE_PARITY_OFF,
				       BEAMFS_RS_PARITY,
				       BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_DATA_INLINE_SUBBLOCKS,
				       rs_results, "file data");
	} else {
		beamfs_rs_decode_region(
			tmp, BEAMFS_SUBBLOCK_TOTAL,
			tmp + BEAMFS_SUBBLOCK_DATA, BEAMFS_SUBBLOCK_TOTAL,
			BEAMFS_SUBBLOCK_DATA, BEAMFS_DATA_INLINE_SUBBLOCKS,
			rs_results,
			rs_positions,
			BEAMFS_RS_PARITY / 2,
			"file data");
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		int rc = rs_results[i];

		if (rc < 0) {
			u32 xflags = rmw_path ?
				BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED : 0;

			/*
			 * Journal the uncorrectable event before raising the
			 * error: forensic record takes priority over the alert.
			 * See Documentation/format-v4.md section 6.5.
			 *
			 * rmw_path=true here means the uncorrectable was
			 * detected during a write transit; the imminent encode
			 * will rewrite the codeword from the in-RAM scratch
			 * buffer, so the on-disk damage is overwritten next
			 * cycle. The journal entry preserves the forensic
			 * record of the event regardless.
			 */
			beamfs_log_rs_event_flagged(sb, (u64)phys,
				NULL, 0,
				BEAMFS_SUBBLOCK_DATA,
				xflags | beamfs_rs_event_subblock_bits(i));
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu subblock=%u uncorrectable%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   i,
					   rmw_path ? " (rmw)" : "");
			beamfs_alert_uncorrectable(sb, phys);
			uncorrectable = true;
		} else if (rc > 0) {
			unsigned int np = (unsigned int)rc;
			int *pos = rs_positions +
				   (size_t)i * (BEAMFS_RS_PARITY / 2);
			u32 xflags = rmw_path ?
				BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED : 0;

			if (np > BEAMFS_RS_PARITY / 2)
				np = BEAMFS_RS_PARITY / 2;
			pr_warn_ratelimited("beamfs/inline: ino=%llu iblock=%llu subblock=%u: %d symbol(s) %s\n",
					    (unsigned long long)inode->i_ino,
					    (unsigned long long)iblock_logical_for_log,
					    i, rc,
					    rmw_path ? "neutralised" : "corrected");
			beamfs_log_rs_event_flagged(sb, (u64)phys,
				pos, np,
				BEAMFS_SUBBLOCK_DATA,
				xflags | beamfs_rs_event_subblock_bits(i));
			/*
			 * The journal says this happened; the budget says
			 * how close the block now is to having nothing left.
			 * A high-water mark, so the byte reports the worst
			 * any subblock of this block has been rather than
			 * the most recent, since the worst is what will
			 * saturate first.
			 */
			beamfs_budget_record(sb, phys, np);
			corrected = true;
		}
	}

	if (uncorrectable) {
		ret = -EIO;
		goto out_brelse;
	}

	/*
	 * DATA_CSUM verification (format-v6). decode_rs8 can return success
	 * while having converged to a wrong valid codeword (silent
	 * miscorrection, known-limitations 3.11). Recompute the payload
	 * checksum and reject on mismatch. The descriptor lives in the tail
	 * pad, which decode does not touch, so it is read from tmp. A NONE
	 * type accepts unconditionally (opt-in / lazy upgrade). On mismatch,
	 * journal an UNCORRECTABLE event and fail closed before any slice
	 * copy, as the per-subblock uncorrectable path does. Observability
	 * mechanism of Theorem v2.2a extended to data regions (v2.2b).
	 */
	if (sbi->s_data_csum) {
		u8  ctype = tmp[BEAMFS_DATA_CSUM_TYPE_OFF];
		u32 want = get_unaligned_le32(tmp + BEAMFS_DATA_CSUM_VALUE_OFF);
		u32 got  = beamfs_inline_payload_crc(tmp);

		/*
		 * csum_type gates the whole check, and the descriptor is
		 * outside every RS codeword, so it is neither corrected nor
		 * detected by the code. Accepting any type other than CRC32 as
		 * "no checksum present" therefore let a single flip on offset
		 * 4080 disable verification for that block, silently. Measured
		 * 2026-08-17 at 128 flips on a 64-block file: 20 RS symbols
		 * corrected, no uncorrectable, no csum mismatch logged, and
		 * 15296 wrong bits returned to userspace across two blocks --
		 * exactly the silent accept that format-v6 section 3.3 claimed
		 * the layout could not produce. That argument holds for the
		 * csum value (a corrupted CRC can only over-reject) but not for
		 * the type byte.
		 *
		 * On a DATA_CSUM volume every data block is stamped at write
		 * time, so any other type is a corrupted descriptor, not an
		 * unstamped block: fail closed instead of waving it through.
		 */
		if (ctype != BEAMFS_CSUM_CRC32) {
			/*
			 * UNCORRECTABLE is reserved: beamfs_log_rs_event_flagged
			 * sets it itself for the positions == NULL, n_positions == 0
			 * call shape used here (format-v4.md section 6.5). Passing it
			 * in extra_flags trips the WARN_ON_ONCE reserved-mask guard at
			 * super.c:523 and the journal entry is dropped, which is what
			 * made the master node report RS_FAILED on 2026-08-24.
			 */
			u32 xflags = 0;

			if (rmw_path)
				xflags |= BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED;

			beamfs_log_rs_event_flagged(sb,
						    (u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS,
						    NULL, 0,
						    BEAMFS_SUBBLOCK_DATA,
						    xflags);
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu data_csum bad descriptor type=0x%02x (expected 0x%02x)%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   ctype, BEAMFS_CSUM_CRC32,
					   rmw_path ? " (rmw)" : "");
			ret = -EIO;
			goto out_brelse;
		}

		if (want != got) {
			beamfs_log_rs_event_flagged(sb,
				(u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS,
				NULL, 0,
				BEAMFS_SUBBLOCK_DATA,
				rmw_path ? BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED : 0);
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu data_csum mismatch want=0x%08x got=0x%08x%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   want, got,
					   rmw_path ? " (rmw)" : "");
			ret = -EIO;
			goto out_brelse;
		}
	}

	/*
	 * DATA_SELFID verification. DATA_CSUM proves the block is intact; it
	 * cannot prove it is the block that was asked for. A corrupted
	 * pointer landing inside the data area reaches a different but valid
	 * block whose own descriptor verifies, so every check above passes on
	 * the wrong data. Measured 2026-08-19: a direct pointer moved from
	 * physical 471 to 503 and the read returned that block's contents,
	 * 15245 wrong bits, with no kernel signal. Comparing the stored
	 * identity digest against the (inode, iblock) actually being read
	 * closes that path.
	 */
	if (sbi->s_data_selfid) {
		u64 want_id = beamfs_data_selfid(inode->i_ino,
						 iblock_logical_for_log);
		u64 got_id  = get_unaligned_le64(tmp + BEAMFS_DATA_SELFID_OFF);

		if (want_id != got_id) {
			/*
			 * UNCORRECTABLE is reserved: beamfs_log_rs_event_flagged
			 * sets it itself for the positions == NULL, n_positions == 0
			 * call shape used here (format-v4.md section 6.5). Passing it
			 * in extra_flags trips the WARN_ON_ONCE reserved-mask guard at
			 * super.c:523 and the journal entry is dropped, which is what
			 * made the master node report RS_FAILED on 2026-08-24.
			 */
			u32 xflags = 0;

			if (rmw_path)
				xflags |= BEAMFS_RS_EVENT_FLAG_RMW_NEUTRALISED;

			beamfs_log_rs_event_flagged(sb,
						    (u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS,
						    NULL, 0,
						    BEAMFS_SUBBLOCK_DATA,
						    xflags);
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu data_selfid mismatch want=0x%016llx got=0x%016llx%s\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   want_id, got_id,
					   rmw_path ? " (rmw)" : "");
			ret = -EIO;
			goto out_brelse;
		}
	}

	/*
	 * Slice gather: copy [slice_offset, slice_offset + slice_length)
	 * of the user-byte view (16 segments of 239 bytes, parity stripped)
	 * into dst_buf. Walk the relevant subblocks and copy the
	 * intersecting portion of each into the destination.
	 */
	{
		u32 slice_end = slice_offset + slice_length;
		u32 sb_first  = slice_offset / BEAMFS_SUBBLOCK_DATA;
		u32 sb_last   = (slice_end - 1) / BEAMFS_SUBBLOCK_DATA;
		u32 dst_off   = 0;
		u32 sb_idx;

		if (BEAMFS_SB(sb)->s_feat_incompat &
		    BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE) {
			/*
			 * A capsule holds its payload contiguously, so
			 * the slice is one copy. Walking it in sixteen
			 * runs 255 apart reads each one 16 bytes further
			 * along than it lies.
			 */
			memcpy(dst_buf + dst_off,
			       tmp + BEAMFS_CAPSULE_DATA_OFF + slice_offset,
			       slice_length);
			dst_off += slice_length;
		} else {
			for (sb_idx = sb_first; sb_idx <= sb_last; sb_idx++) {
				u32 sb_user_start = sb_idx * BEAMFS_SUBBLOCK_DATA;
				u32 sb_user_end   = sb_user_start +
						    BEAMFS_SUBBLOCK_DATA;
				u32 from_in_sb    = (slice_offset > sb_user_start)
					? (slice_offset - sb_user_start) : 0;
				u32 to_in_sb      = (slice_end < sb_user_end)
					? (slice_end - sb_user_start)
					: BEAMFS_SUBBLOCK_DATA;
				u32 copy_len      = to_in_sb - from_in_sb;

				memcpy(dst_buf + dst_off,
				       tmp
					+ (size_t)sb_idx * BEAMFS_SUBBLOCK_TOTAL
					+ from_in_sb,
				       copy_len);
				dst_off += copy_len;
			}
		}
	}

	/*
	 * Durable autonomic repair: TEMPORARILY DISABLED.
	 *
	 * Previously this path wrote corrected bytes back to disk via
	 * mark_buffer_dirty + sync_dirty_buffer. With the decode-into-
	 * tmp fix above, bh->b_data is no longer the decoded buffer
	 * (tmp is). Re-enabling repair requires either:
	 *  (a) an exclusive lock on phys around the read-modify-write
	 *      window, OR
	 *  (b) a CoW write path that allocates a fresh phys and
	 *      updates the indirect pointer.
	 * Deferred to follow-up. RS correction is still detected and
	 * journalled (see corrected=true path above), only the on-disk
	 * repair is skipped.
	 */
	(void)corrected;

	beamfs_scratch_put(sb, tmp);
	return 0;

out_brelse:
	beamfs_scratch_put(sb, tmp);
	return ret;
}

/*
 * beamfs_inline_decode_symlink -- RS-decode a block holding a link target.
 *
 * The same decode as file data, exposed for namei.c because a long
 * symlink is a data block in every respect but the path that reaches it.
 * Separate entry point rather than a wider signature on the file path:
 * this one takes no folio, no iomap iterator and no RMW flag, and
 * threading three unused arguments through the reader to avoid twelve
 * lines here would be the worse trade.
 */
int beamfs_inline_decode_symlink(struct super_block *sb,
				 struct buffer_head *bh, u64 phys,
				 struct inode *inode, u8 *dst, u32 len)
{
	if (len > BEAMFS_DATA_INLINE_BYTES)
		return -EUCLEAN;

	return beamfs_inline_decode_block_into_buf(sb, bh, phys, inode, 0,
						   dst, 0, len, false);
}


/* ------------------------------------------------------------------------- */
/* beamfs_inline_folio_coverage -- compute INLINE disk block coverage of a   */
/*                                  VFS folio.                               */
/*                                                                           */
/* The fundamental impedance: VFS folio carries PAGE_SIZE (4096) user bytes  */
/* per index, INLINE disk block carries BEAMFS_DATA_INLINE_BYTES (3824) user */
/* bytes. Therefore a folio at index N spans user-byte range                 */
/*   [N * PAGE_SIZE, (N+1) * PAGE_SIZE)                                      */
/* and intersects 1, 2, or 3 consecutive INLINE disk blocks. Tri-block       */
/* coverage occurs when k_first > 2*INLINE_BYTES - PAGE_SIZE (= 3552), at    */
/* folio indices N where (N * PAGE_SIZE) mod INLINE_BYTES > 3552. With       */
/* INLINE_BYTES=3824 and PAGE_SIZE=4096, this is periodic with period 14    */
/* (folios N=14, 28, 42, ...).                                              */
/*                                                                           */
/* Outputs:                                                                  */
/*   *out_b_first         disk block index covering folio_start_byte         */
/*   *out_k_first         byte offset within b_first where folio begins      */
/*   *out_b_last          disk block index covering folio_end_byte - 1       */
/*                        (== b_first or b_first + 1)                        */
/*   *out_len_in_b_last   bytes within b_last that the folio covers          */
/*   *out_folio_user_bytes total user bytes in this folio (1..PAGE_SIZE)     */
/*                                                                           */
/* Invariants on success:                                                    */
/*   out_b_first <= out_b_last <= out_b_first + 2                            */
/*   1 <= out_len_in_b_last <= BEAMFS_DATA_INLINE_BYTES                      */
/*   out_k_first < BEAMFS_DATA_INLINE_BYTES                                  */
/*   if b_last == b_first: out_folio_user_bytes == out_len_in_b_last         */
/*   else if b_last == b_first + 1: out_folio_user_bytes ==                  */
/*           (BEAMFS_DATA_INLINE_BYTES - k_first) + out_len_in_b_last        */
/*   else (b_last == b_first + 2): out_folio_user_bytes ==                   */
/*           (BEAMFS_DATA_INLINE_BYTES - k_first) + BEAMFS_DATA_INLINE_BYTES */
/*           + out_len_in_b_last                                             */
/*                                                                           */
/* Returns 0 on success, -ERANGE if folio_index is at or beyond i_size       */
/* (caller should zero-fill and end_read in that case, per VFS).             */
/*                                                                           */
/* Pure integer arithmetic. No locks, no allocations. Validated out-of-band  */
/* on 15 boundary cases (see commit message and INLINE-MULTIBLOCK-DESIGN.md  */
/* section 1.4).                                                             */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* read_folio (v2 INLINE) -- per-block RS(255,239) FEC, multi-block scope.   */
/*                                                                           */
/* Convention C (sliding window, VFS-conformant): folio at index N maps to   */
/* user-byte range [N * PAGE_SIZE, (N+1) * PAGE_SIZE), and beamfs translates */
/* this to one or two INLINE disk blocks at runtime via folio_coverage.      */
/*                                                                           */
/* Steps:                                                                    */
/*   1) WARN if folio is not single-page (mapping_set_folio_order_range)     */
/*   2) folio_coverage -> (b_first, k_first, b_last, len_in_b_last, fub)     */
/*      ERANGE: folio beyond i_size -> zero-fill, end_read, return 0         */
/*   3) for each disk block b in [b_first, b_last]:                          */
/*       a) compute slice (offset within block, length to copy into folio)   */
/*       b) lookup_phys for b                                                */
/*       c) HOLE  -> memset zero the slice in the folio                      */
/*          else  -> decode_block_into_buf with the slice                    */
/*   4) zero-pad folio bytes [folio_user_bytes, BEAMFS_BLOCK_SIZE)           */
/*   5) flush_dcache + folio_end_read                                        */
/*                                                                           */
/* HOLE handling is per-block: a multi-block folio with only one HOLE block  */
/* gets the corresponding slice zeroed and the other slice decoded normally. */
/* This is the sparse-file case (lseek + write past i_size).                 */
/*                                                                           */
/* MIL-STD-883 SEE coverage: validates beamfs resistance to RadFI            */
/* single-bit and multi-byte payload corruption injected at submit_bio,      */
/* across multi-block files (the v2.x WOW-factor target).                    */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* iomap read path                                                           */
/*                                                                           */
/* The INLINE format interleaves data and parity inside every 4096-byte      */
/* block: 16 RS(255,239) subblocks of [239 data][16 parity]. 3824 logical    */
/* bytes therefore occupy 4096 physical bytes, and file offset is not disk   */
/* offset plus a constant -- which is what iomap_sector() assumes.           */
/*                                                                           */
/* Two consequences shape this code. iomap_begin maps one INLINE block per   */
/* iteration (length = BEAMFS_DATA_INLINE_BYTES) so the affine assumption    */
/* holds within a mapping; iomap iterates over the rest. And the read is     */
/* done by beamfs_inline_read_folio_range() rather than by                   */
/* iomap_bio_read_ops, because the bytes that reach the page cache are not   */
/* the bytes on disk: they are what RS decoding produces from them.          */
/* ------------------------------------------------------------------------- */

static int beamfs_inline_iomap_begin(struct inode *inode, loff_t pos,
				     loff_t length, unsigned int flags,
				     struct iomap *iomap,
				     struct iomap *srcmap)
{
	loff_t i_size = i_size_read(inode);
	u32 payload = beamfs_block_payload(inode->i_sb);
	u64 b = (u64)pos / payload;
	u64 phys = 0;
	int ret;

	iomap->flags = 0;
	iomap->bdev = inode->i_sb->s_bdev;
	iomap->addr = IOMAP_NULL_ADDR;

	/*
	 * Map the range the caller asked for, clamped to the end of the
	 * INLINE block that backs it -- not a fixed 3824-byte window
	 * starting at a block boundary. iomap tracks its position within
	 * the folio against the mapping it was given, and readahead ends
	 * a folio when offset_in_folio(folio, pos) reaches zero; handing
	 * back a range that starts before the requested position leaves
	 * that comparison never matching, and the iterator walks off a
	 * folio it has already released.
	 */
	iomap->offset = pos;

	/*
	 * Past EOF the whole remaining range is a hole, in one mapping:
	 * iomap zero-fills it without calling back. Splitting it into
	 * INLINE-sized pieces would leave iomap iterating after
	 * iomap_read_folio_iter() has released the folio, which
	 * dereferences NULL on the next round.
	 */
	/*
	 * Past EOF is a hole to a reader and nothing of the sort to a
	 * writer.
	 *
	 * A write at pos == i_size is the ordinary case of extending a
	 * file, and the block holding that position usually exists
	 * already: at 3824 usable bytes per block, appending to a 32-byte
	 * file writes into block 0, which has data in it.
	 *
	 * Reporting IOMAP_HOLE there made iomap_block_needs_zeroing true
	 * on the first term, so __iomap_write_begin took the zeroing
	 * branch and called folio_zero_segments over the whole folio
	 * outside the written range -- erasing bytes 0..31, which were on
	 * disk and below i_size. generic/639 writes 32 bytes, cycles the
	 * mount, writes 32 more, and reads back thirty-two zeros followed
	 * by the second write. Silent loss of data the caller never
	 * touched.
	 *
	 * iomap works in i_blocksize units, 4096, while a block carries
	 * 3824 bytes of payload, so its block boundaries are not ours and
	 * a range it considers past EOF can hold live bytes. Answering
	 * the question it actually asked -- what is mapped here -- rather
	 * than a shortcut about EOF keeps the two views consistent.
	 */
	if (!(flags & IOMAP_WRITE) && pos >= i_size) {
		iomap->length = length;
		iomap->type = IOMAP_HOLE;
		return 0;
	}

	/*
	 * One mapping covers the whole requested range, even where that
	 * spans two or three INLINE blocks.
	 *
	 * iomap_read_folio_iter() releases the folio as soon as a call
	 * returns with no iomap_folio_state attached -- which is always
	 * the case here, folio size and block size both being 4096, so
	 * ifs_alloc() has nothing to track. A mapping shorter than the
	 * folio therefore leaves iomap iterating over a folio it has
	 * already given up, and the next round dereferences NULL at
	 * folio_size(). Verified by disassembly: iomap_read_folio_iter+0x7c
	 * is mov (%r12),%rax with r12 = ctx->cur_folio.
	 *
	 * Spanning blocks is fine because read_folio_range does its own
	 * reading: it walks the blocks the range covers and decodes each.
	 * iomap->addr is reported to fiemap and tracing but nothing
	 * derives an I/O sector from it.
	 */
	/*
	 * Report asks about one block; read asks about the whole range.
	 *
	 * IOMAP_REPORT is fiemap, and it wants the truth: this mapping
	 * describes the block pos falls in and nothing beyond it.
	 * Claiming the requested length told fiemap that the answer for
	 * the first block held for the entire file, so a leading hole
	 * was reported as spanning 1 MiB, iomap stepped past EOF, and a
	 * file with 512 KiB of data came back with no extents at all.
	 *
	 * Everything else keeps the full length, and must. A mapping
	 * shorter than the folio makes iomap_read_folio_iter release the
	 * folio -- nothing attached an iomap_folio_state, folio and
	 * block size both being 4096 -- and then dereference it on the
	 * next round at folio_size(). That is the NULL documented below,
	 * confirmed by disassembly, and a 3824-byte mapping is always
	 * shorter than a 4096-byte folio.
	 */
	if (flags & IOMAP_REPORT) {
		u64 in_block = (u64)pos % payload;

		iomap->length = min_t(u64, (u64)length,
				      BEAMFS_DATA_INLINE_BYTES - in_block);
	} else {
		iomap->length = length;
	}

	/*
	 * A write allocates here, not at writeback.
	 *
	 * Only lookup_phys was called, for every caller, so a write to an
	 * unallocated block was told IOMAP_HOLE and accepted into the page
	 * cache; the allocation happened later in the flusher, where an
	 * ENOSPC has nobody left to report to. A volume at 100% took a
	 * 10 MiB write at 5.5 GB/s -- nothing reached the disk, and the
	 * application was told it had.
	 *
	 * That is silent data loss, on a filesystem whose whole purpose is
	 * that data survives.
	 *
	 * It also explains the EFBIG that ended generic/015 and
	 * generic/269: dd kept writing past the end of a full 1 GiB volume
	 * until the file hit the 134480396-block ceiling of the
	 * indirection tree -- 479 GiB of file on a gigabyte of disk --
	 * because no write ever failed.
	 *
	 * ext2 draws the same line: create = flags & IOMAP_WRITE.
	 */
	if (flags & IOMAP_WRITE) {
		struct beamfs_inode_info *fi = BEAMFS_I(inode);

		/*
		 * Allocation is serialised by i_alloc_mutex, and this is
		 * an allocation site.
		 *
		 * The two other callers take it; this one did not, so two
		 * threads writing the same logical block each allocated a
		 * physical one and the second overwrote the first's
		 * pointer. The overwritten block stayed marked used with
		 * nothing referencing it -- 36 leaked out of 317559
		 * allocations under generic/344, which runs holetest with
		 * 256 threads on one sparse file. A narrow window, hit
		 * often enough by 256 threads.
		 */
		bool fresh = false;

		mutex_lock(&fi->i_alloc_mutex);
		ret = beamfs_inline_lookup_or_alloc_phys_new(inode, b, &phys,
							     &fresh);
		mutex_unlock(&fi->i_alloc_mutex);

		/*
		 * IOMAP_F_NEW: this mapping did not exist before the call.
		 *
		 * iomap_end may only give back what this call created. A
		 * block the file already owned belongs to it whether or
		 * not the write that asked for the mapping reached it, and
		 * freeing that would lose data nobody asked to lose.
		 */
		if (ret == 0 && fresh)
			iomap->flags |= IOMAP_F_NEW;
	} else {
		ret = beamfs_inline_lookup_phys(inode, b, &phys);
	}
	/*
	 * The last place a wrong address can still be stopped.
	 *
	 * What goes into the iomap below becomes a bio, and a bio does
	 * not ask where it is going: an address under s_data_start lands
	 * on the inode table. generic/075 put fsx's fill bytes in blocks
	 * 7 and 246 that way and left no trace in any buffer head,
	 * because iomap does not use them.
	 *
	 * Checked here rather than at each of the seventeen places the
	 * two lookups return a block: one gate the address must pass, and
	 * one place to read when it does not.
	 */
	if (ret == 0 &&
	    !beamfs_phys_is_sane(inode->i_sb, inode, phys, "iomap_begin"))
		ret = -EUCLEAN;

	if (ret < 0)
		return ret;

	if (phys == 0) {
		iomap->type = IOMAP_HOLE;
		return 0;
	}

	iomap->type = IOMAP_MAPPED;
	/*
	 * Physical block start. Nothing derives a sector from it for I/O --
	 * read_folio_range reads the block itself -- but fiemap and the
	 * tracepoints report it, so it has to be the real address.
	 */
	iomap->addr = (u64)phys * BEAMFS_BLOCK_SIZE;

	(void)srcmap;
	return 0;
}

static const struct iomap_ops beamfs_inline_iomap_ops = {
	.iomap_begin = beamfs_inline_iomap_begin,
};

/*
 * Read one mapped range into the folio, decoding it on the way.
 *
 * iomap has already resolved which INLINE block backs this file range;
 * what is left is to fetch the physical block, run the 16 RS codewords
 * through the decoder, and copy out the slice the folio asked for. The
 * read is synchronous, as iomap_bio_read_folio_range_sync() shows is
 * expected, so iomap_finish_folio_read() is called before returning.
 */
static int beamfs_inline_read_folio_range(const struct iomap_iter *iter,
					  struct iomap_read_folio_ctx *ctx,
					  size_t len)
{
	struct inode       *inode = iter->inode;
	struct super_block *sb    = inode->i_sb;
	struct folio       *folio = ctx->cur_folio;
	loff_t              i_size = i_size_read(inode);
	u64                 pos   = (u64)iter->pos;
	u64                 end   = pos + len;
	size_t              done  = 0;
	int                 ret   = 0;

	/*
	 * The range can straddle INLINE blocks, so walk them: for each,
	 * read the physical block, run its 16 RS codewords through the
	 * decoder, and copy out the slice this folio wants.
	 */
	/*
	 * Past EOF within this range: zero it here rather than returning a
	 * second HOLE mapping. iomap releases the folio as soon as a call
	 * returns without an iomap_folio_state attached -- always the case
	 * at 4096-byte folios and blocks -- so a folio must be covered by
	 * exactly one mapping. Traced on 2026-08-29: a 1000-byte file
	 * produced MAPPED [0, 0x3e8) then HOLE [0x3e8, 0x1000), and the
	 * second iteration dereferenced the released folio.
	 */
	if (end > (u64)i_size) {
		u64 eof = max_t(u64, pos, (u64)i_size);

		folio_zero_range(folio, offset_in_folio(folio, eof),
				 (size_t)(end - eof));
		end = eof;
	}

	while (pos < end) {
		u32    payload      = beamfs_block_payload(inode->i_sb);
		u64    b            = pos / payload;
		u32    slice_offset = (u32)(pos % payload);
		u32    slice_length = (u32)min_t(u64, end - pos,
						 payload - slice_offset);
		size_t folio_off    = offset_in_folio(folio, pos);
		struct buffer_head *bh;
		u64    phys = 0;
		u8    *dst;

		/*
		 * The tree read under the allocator's mutex.
		 *
		 * lookup_phys walks i_direct and the indirect blocks while
		 * writeback, truncate and eviction are free to rewrite them.
		 * Reading a pointer mid-update returns a block number that
		 * belongs to no one, or to another file once it has been
		 * reallocated. ext2 holds truncate_mutex across the same walk.
		 *
		 * Held for the lookup only, released before the read that
		 * follows: the block I/O may sleep and has no business
		 * holding up the allocator.
		 */
		mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
		ret = beamfs_inline_lookup_phys(inode, b, &phys);
		mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);
		if (ret < 0)
			break;

		if (phys == 0) {
			folio_zero_range(folio, folio_off, slice_length);
			pos  += slice_length;
			done += slice_length;
			continue;
		}

		/*
		 * sb_bread, and it can loop.
		 *
		 * __bread_gfp adds __GFP_NOFAIL whatever mask it is
		 * given -- fs/buffer.c line 1410, "prefer looping in the
		 * allocator rather than here" -- so there is no mask that
		 * makes this read fail cleanly. Passing __GFP_NORETRY was
		 * tried and does nothing.
		 *
		 * generic/464 wedges here: page allocation stall under
		 * bdev_getblk, called from a page fault with the folio
		 * locked, on a machine whose page cache is full of this
		 * filesystem's own blocks. The way out is not to reach
		 * the buffer cache from the fault path at all, which is
		 * what ext2 under iomap does and what this still has to
		 * do.
		 */
		bh = sb_bread(sb, phys);
		if (!bh) {
			pr_err_ratelimited("beamfs/inline: read_folio_range: sb_bread phys=%llu failed\n",
					   (unsigned long long)phys);
			ret = -EIO;
			break;
		}

		dst = kmap_local_folio(folio, folio_off);
		lock_buffer(bh);
		ret = beamfs_inline_decode_block_into_buf(sb, bh, phys, inode,
							  b, dst,
							  slice_offset,
							  slice_length, false);
		unlock_buffer(bh);
		kunmap_local(dst);
		brelse(bh);
		if (ret < 0)
			break;

		pos  += slice_length;
		done += slice_length;
	}

	/*
	 * On failure, return without finishing the folio.
	 * iomap_read_folio_iter() propagates a non-zero return before
	 * counting the range as submitted, and iomap_read_folio() then
	 * ends the folio itself through iomap_read_end(). Calling
	 * iomap_finish_folio_read() here as well runs folio_end_read()
	 * on a folio iomap still owns -- with no iomap_folio_state
	 * attached, which is always the case at 4096-byte folios and
	 * blocks, that call is unconditional -- and the second end
	 * leaves any waiter parked in folio_wait_bit_common() forever.
	 *
	 * Observed on 2026-08-30 by the saturation protocol: a block with
	 * nine symbol errors in one subblock was correctly detected and
	 * journalled, but the read never returned EIO -- cat sat in
	 * uninterruptible sleep instead. The fail-closed half of the
	 * saturation contract was the casualty, not the detection half.
	 *
	 * iomap_bio_read_folio_range_sync() has the same shape: it
	 * returns the error and finishes nothing.
	 */
	if (ret < 0)
		return ret;

	iomap_finish_folio_read(folio, offset_in_folio(folio, iter->pos),
				len, 0);
	(void)done;
	return 0;
}

static const struct iomap_read_ops beamfs_inline_read_ops = {
	.read_folio_range = beamfs_inline_read_folio_range,
};

/*
 * No .readahead. iomap_readahead_iter() moves to the next folio only when
 * offset_in_folio(cur_folio, iter->pos) reaches zero, so it assumes
 * mappings line up with folio boundaries. INLINE mappings end on 3824-byte
 * block boundaries and folios are 4096 bytes, so that comparison never
 * matches at the right moment: iomap_read_folio_iter() clears cur_folio
 * once the folio is full, the transition to the next folio does not fire,
 * and the following iteration dereferences NULL. Confirmed twice on
 * kernel 7.1.3.
 *
 * readahead is optional in address_space_operations. Aligning the format
 * to folios would mean moving parity out of the data block, which is what
 * lets a single read correct a flip without depending on a second block --
 * the property the filesystem exists for. Sequential reads simply go one
 * folio at a time through read_folio.
 *
 * read_folio: iomap resolves which INLINE block backs each
 * file range, then calls beamfs_inline_read_folio_range() to produce the
 * bytes. The folio locking, uptodate accounting and readahead batching
 * are iomap's; what stays here is the RS decode.
 */
static int beamfs_inline_read_folio(struct file *file, struct folio *folio)
{
	struct iomap_read_folio_ctx ctx = {
		.ops       = &beamfs_inline_read_ops,
		.cur_folio = folio,
	};

	iomap_read_folio(&beamfs_inline_iomap_ops, &ctx, NULL);
	return 0;
}



/* ------------------------------------------------------------------------- */
/* readahead -- per-folio loop on top of read_folio.                         */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* write_begin (v2 INLINE, multi-block scope)                                */
/*                                                                           */
/* Provides a folio for the write to land into. The actual encoding to disk  */
/* (RS encode + sync) happens in writepages.                                 */
/*                                                                           */
/* MULTI-BLOCK SCOPE: pos+len may span across a 4096-byte folio boundary    */
/* into 1 or 2 underlying INLINE disk blocks (3824 user bytes per block).   */
/* RMW via read_folio populates the folio with the existing data of the     */
/* covered block(s) before the user write lands; writepages later RS-encodes*/
/* and writes back the covered blocks. See INLINE-MULTIBLOCK-DESIGN.md S2.3.*/
/*                                                                           */
/* RMW handling:                                                             */
/*   - If the folio is already uptodate, no read is needed (overwrite).      */
/*   - Otherwise, look up the physical block. If allocated, read+decode      */
/*     it via the existing read path. If HOLE, zero-fill the folio.          */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* iomap write path                                                          */
/*                                                                           */
/* Same shape as the read side. iomap owns the folio lifecycle -- locking,   */
/* dirty accounting, writeback tagging -- and beamfs owns what the bytes     */
/* look like on disk: the read-modify-write cycle that decodes a block,      */
/* splices the folio's contribution in, re-encodes all 16 RS codewords and   */
/* stamps the DATA_CSUM descriptor.                                          */
/* ------------------------------------------------------------------------- */

/*
 * Fill a folio range for a partial write. iomap calls this when a write
 * covers only part of a folio that is not uptodate, so the rest has to
 * come off disk first. The contract requires a synchronous read, which
 * is what the decode path does anyway.
 */
static int beamfs_inline_write_read_folio_range(const struct iomap_iter *iter,
						struct folio *folio,
						loff_t pos, size_t len)
{
	struct inode       *inode = iter->inode;
	struct super_block *sb    = inode->i_sb;
	loff_t              i_size = i_size_read(inode);
	u64                 p     = (u64)pos;
	u64                 end   = p + len;
	int                 ret   = 0;

	if (end > (u64)i_size) {
		u64 eof = max_t(u64, p, (u64)i_size);

		folio_zero_range(folio, offset_in_folio(folio, eof),
				 (size_t)(end - eof));
		end = eof;
	}

	while (p < end) {
		u32    payload      = beamfs_block_payload(inode->i_sb);
		u64    b            = p / payload;
		u32    slice_offset = (u32)(p % payload);
		u32    slice_length = (u32)min_t(u64, end - p,
						 payload - slice_offset);
		size_t folio_off    = offset_in_folio(folio, p);
		struct buffer_head *bh;
		u64    phys = 0;
		u8    *dst;

		/*
		 * The tree read under the allocator's mutex.
		 *
		 * lookup_phys walks i_direct and the indirect blocks while
		 * writeback, truncate and eviction are free to rewrite them.
		 * Reading a pointer mid-update returns a block number that
		 * belongs to no one, or to another file once it has been
		 * reallocated. ext2 holds truncate_mutex across the same walk.
		 *
		 * Held for the lookup only, released before the read that
		 * follows: the block I/O may sleep and has no business
		 * holding up the allocator.
		 */
		mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
		ret = beamfs_inline_lookup_phys(inode, b, &phys);
		mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);
		if (ret < 0)
			break;

		if (phys == 0) {
			folio_zero_range(folio, folio_off, slice_length);
			p += slice_length;
			continue;
		}

		bh = sb_bread(sb, phys);
		if (!bh) {
			ret = -EIO;
			break;
		}

		dst = kmap_local_folio(folio, folio_off);
		lock_buffer(bh);
		ret = beamfs_inline_decode_block_into_buf(sb, bh, phys, inode,
							  b, dst, slice_offset,
							  slice_length, false);
		unlock_buffer(bh);
		kunmap_local(dst);
		brelse(bh);
		if (ret < 0)
			break;

		p += slice_length;
	}

	return ret;
}

static const struct iomap_write_ops beamfs_inline_write_ops = {
	.read_folio_range = beamfs_inline_write_read_folio_range,
};


/*
 * Writeback completion for the last block of a folio's range.
 *
 * iomap_writeback_folio() ends the folio itself only when nothing was
 * submitted; past that it waits for the filesystem to report
 * completion. submit_bh is asynchronous, so the folio cannot be
 * finished when writeback_range returns -- the write is still in
 * flight. One block of the range carries this handler and finishes the
 * folio when its write lands; the rest get end_buffer_write_sync.
 */
struct beamfs_inline_wb_ctx {
	struct inode  *inode;
	struct folio  *folio;
	size_t         len;
	void          *orig_private;
};

/*
 * Completion for a writeback block.
 *
 * Takes a bio rather than a buffer_head: b_end_io went away in 7.3 and
 * the completion is passed to bh_submit() instead of being stored on
 * the buffer. bio_endio_bh() hands back the buffer the bio was built
 * around, which is what still makes a filesystem's own completion
 * possible.
 *
 * The old version saved and restored b_end_io. With the field gone
 * there is nothing to put back, which is one fewer thing to get wrong.
 */
static void beamfs_inline_wb_end_io(struct bio *bio)
{
	struct buffer_head *bh;
	bool uptodate = bio_endio_bh(bio, &bh);
	struct beamfs_inline_wb_ctx *wb = bh->b_private;
	struct inode *inode = wb->inode;
	struct folio *folio = wb->folio;
	size_t len = wb->len;

	bh->b_private = wb->orig_private;
	kfree(wb);

	if (!uptodate)
		mapping_set_error(inode->i_mapping, -EIO);

	/*
	 * What end_buffer_write_sync did: record the error on the
	 * buffer, mark it uptodate or not, unlock it, and drop the
	 * reference taken before submission.
	 */
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		mark_buffer_write_io_error(bh);
		clear_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
	put_bh(bh);

	iomap_finish_folio_write(inode, folio, len);
}

/*
 * Write back one range of a folio.
 *
 * For each INLINE block the range touches: decode what is on disk into
 * scratch, splice this folio's bytes over it, re-encode all 16 codewords,
 * stamp the descriptor, and hand the buffer to the block layer.
 *
 * The decode uses rmw_path=true so a flip found on disk here is journalled
 * as RMW_NEUTRALISED: the encode below overwrites the damage from the
 * in-RAM scratch before any reader can see it.
 *
 * The buffer_head lock is held across the whole decode-splice-encode
 * transit. bh->b_data is shared through the block device page cache, and
 * two writeback paths on the same physical block -- the bdi flusher and an
 * fsync, or two folios sharing an intermediate block in the tri-block case
 * -- would otherwise interleave their re-scatter and encode steps, leaving
 * a corrupt codeword on disk while both page cache copies still look
 * uptodate. That is the "hot sha == cold sha mismatch" signature.
 */
static ssize_t beamfs_inline_writeback_range(struct iomap_writepage_ctx *wpc,
					     struct folio *folio, u64 pos,
					     unsigned int len, u64 end_pos)
{
	struct inode             *inode = wpc->inode;
	struct super_block       *sb    = inode->i_sb;
	struct beamfs_inode_info *fi    = BEAMFS_I(inode);
	u64      p    = pos;
	u64      end  = pos + len;
	u8      *scratch;
	u64      last_phys = 0;
	ssize_t  done = 0;
	int      ret  = 0;

	/*
	 * One batch per call. Carrying it in wpc->wb_ctx would group
	 * across folios too, which is where the remaining factor of two
	 * lives; this keeps the lifetime plain while the change is new,
	 * because a bio that outlives its call is a bio nobody submits.
	 */
	struct beamfs_inline_wb_ctx *wb = NULL;
	struct buffer_head *last_bh = NULL;
	bool     folio_done = false;

	/*
	 * 3824 bytes will not fit on the aarch64 kernel stack given the
	 * frame pressure this path already carries.
	 */
	/*
	 * Tell iomap this range is mapped.
	 *
	 * iomap_writeback_range() adds the return value to
	 * bytes_submitted only when wpc->iomap.type is not IOMAP_HOLE:
	 *
	 *   if (wpc->iomap.type != IOMAP_HOLE)
	 *           *bytes_submitted += ret;
	 *
	 * Nothing here ever filled wpc->iomap in, so it stayed zeroed --
	 * and IOMAP_HOLE is zero. Every folio therefore came out of
	 * iomap_writeback_folio() with bytes_submitted == 0, whatever had
	 * actually been written, and iomap took the "nothing was
	 * submitted" branch and ended the writeback itself. The
	 * completion armed below then ended it a second time: 74
	 * folio_end_writeback for 60 folios, measured.
	 *
	 * The hang follows from the double end. Between iomap's end and
	 * the late completion, another thread can redirty the folio and
	 * start writeback again; the stale completion ends that one, and
	 * the writeback it belonged to is left with nobody to close it.
	 * rm then sits in folio_wait_writeback under evict_inode for as
	 * long as the mount lasts.
	 *
	 * XFS fills this in through xfs_bmbt_to_iomap for the same
	 * reason. Here the mapping is per-block and computed inside the
	 * loop, so the type is all iomap needs: it uses it to decide
	 * whether the range counted, not to find the blocks.
	 */
	wpc->iomap.type = IOMAP_MAPPED;
	wpc->iomap.offset = pos;
	wpc->iomap.length = len;
	wpc->iomap.bdev = sb->s_bdev;

	scratch = beamfs_scratch_get(sb);
	if (!scratch) {
		/*
		 * Finish the folio before leaving. Returning straight out
		 * left it in writeback with no completion coming, and the
		 * next process to touch the file waited on it forever --
		 * the same shape as the defects above, reached by a path
		 * that only opens when memory is short.
		 *
		 * The allocator has already logged the failure, so there
		 * is nothing to add to it here.
		 */
		iomap_finish_folio_write(inode, folio, len);
		return -ENOMEM;
	}

	while (p < end) {
		u32    payload      = beamfs_block_payload(inode->i_sb);
		u64    b            = p / payload;
		u32    slice_offset = (u32)(p % payload);
		u32    slice_length = (u32)min_t(u64, end - p,
						 payload - slice_offset);
		size_t folio_off    = offset_in_folio(folio, p);
		struct buffer_head *bh;
		u64    phys = 0;
		u8    *src;

		{
			u64 before = 0;

			mutex_lock(&fi->i_alloc_mutex);
			(void)beamfs_inline_lookup_phys(inode, b, &before);
			ret = beamfs_inline_lookup_or_alloc_phys(inode, b,
								 &phys);
			mutex_unlock(&fi->i_alloc_mutex);
			if (ret < 0)
				break;
			/*
			 * Sonde : un bloc deja mappe qui change d adresse
			 * signifie que le precedent vient d etre perdu.
			 */
			if (before && phys && before != phys)
				pr_err("beamfs/leak: ino=%llu iblock=%llu %llu -> %llu\n",
				       (unsigned long long)inode->i_ino, (unsigned long long)b,
				       (unsigned long long)before,
				       (unsigned long long)phys);
		}

		/*
		 * Never lock the same buffer twice in one pass.
		 *
		 * sb_bread returns the same buffer_head for the same block,
		 * so a second visit to a physical block already handled in
		 * this loop deadlocks the flusher against itself in
		 * lock_buffer, with no timeout and no way out. umount then
		 * waits behind it in wb_wait_for_completion and the
		 * filesystem is unusable for the life of the mount.
		 *
		 * It should not be possible: distinct logical blocks map to
		 * distinct physical ones. It happened under generic/027,
		 * which fills the volume to ENOSPC and unlinks in a loop,
		 * so the mapping is wrong somewhere upstream -- two logical
		 * blocks resolving to one physical block is corruption
		 * whatever produced it.
		 *
		 * Refusing to proceed turns a permanent hang into an EIO
		 * the caller can see, and says so loudly enough to find the
		 * real cause. Fixing the mapping is a separate matter; this
		 * is about the flusher not being the thing that dies.
		 */
		if (phys == last_phys) {
			pr_err_ratelimited("beamfs/inline: ino=%llu iblock=%llu maps to phys=%llu again in one writeback pass\n",
					   (unsigned long long)inode->i_ino,
					   (unsigned long long)b,
					   (unsigned long long)phys);
			ret = -EIO;
			break;
		}
		last_phys = phys;

		/*
		 * A block being written whole needs a buffer, not its
		 * contents.
		 *
		 * sb_bread reads from the disk and waits for it. On the
		 * sequential path that is one synchronous read per 3824
		 * bytes written, and the flusher spends its time waiting
		 * for data it is about to discard: draining 300 MiB of
		 * dirty pages meant 80000 blocking reads, one after
		 * another. Measured with PSI at 60% of wall time stalled on
		 * I/O with 319 MiB dirty and never draining -- generic/074
		 * and generic/015 both timed out that way, and neither was
		 * stuck.
		 *
		 * sb_getblk returns the same buffer_head with no read at
		 * all. The block is filled entirely below and marked
		 * uptodate, which is what ext4 does on the same path.
		 *
		 * The partial case still reads: those bytes are kept.
		 */
		if (slice_offset == 0 &&
		    slice_length == BEAMFS_DATA_INLINE_BYTES) {
			bh = sb_getblk(sb, phys);
			if (!bh) {
				pr_err_ratelimited("beamfs/inline: writeback_range: sb_getblk phys=%llu failed\n",
						   (unsigned long long)phys);
				ret = -EIO;
				break;
			}
		} else {
			bh = sb_bread(sb, phys);
			if (!bh) {
				pr_err_ratelimited("beamfs/inline: writeback_range: sb_bread phys=%llu failed\n",
						   (unsigned long long)phys);
				ret = -EIO;
				break;
			}
		}

		/*
		 * Send the batch before waiting on a buffer lock.
		 *
		 * Up to 32 buffers stay locked until the bio is submitted,
		 * so a block that comes round again inside the same pass
		 * finds its own lock held and waits for a completion that
		 * cannot arrive until this loop submits -- which it never
		 * will. The flusher sat in __lock_buffer for an hour with
		 * zero writes and PSI at 80%, and every fsstress behind it
		 * blocked in sync_inodes_sb.
		 *
		 * The old phys == last_phys guard only caught the same
		 * block twice in a row, not one seen 30 positions earlier.
		 *
		 * trylock first: on the common path it succeeds and costs
		 * nothing. On contention, submit what is held -- which
		 * releases those buffers through the completion -- and
		 * then wait properly.
		 */
		lock_buffer(bh);

		/*
		 * Read-modify-write, except when there is nothing to
		 * preserve.
		 *
		 * The decode exists so a partial write keeps the bytes it
		 * does not touch. A slice covering the whole block leaves
		 * none of them: every byte of scratch is overwritten two
		 * statements later, so decoding first is 16 RS passes over
		 * data that is discarded.
		 *
		 * That is the common case -- a sequential writer fills
		 * block after block end to end -- and decode_rs8 was 5% of
		 * a write-only profile because of it.
		 *
		 * A block being written whole also does not need its
		 * previous contents to be correctable: an uncorrectable
		 * block that is about to be entirely replaced is not an
		 * error, and reporting it as one would be wrong.
		 */
		if (slice_offset == 0 &&
		    slice_length == BEAMFS_DATA_INLINE_BYTES) {
			src = kmap_local_folio(folio, folio_off);
			memcpy(scratch, src, slice_length);
			kunmap_local(src);
		} else {
			ret = beamfs_inline_decode_block_into_buf(sb, bh, phys,
								  inode, b,
								  scratch, 0,
								  BEAMFS_DATA_INLINE_BYTES,
								  true);
			if (ret < 0) {
				unlock_buffer(bh);
				brelse(bh);
				break;
			}

			src = kmap_local_folio(folio, folio_off);
			memcpy(scratch + slice_offset, src, slice_length);
			kunmap_local(src);
		}

		beamfs_lay_payload(sb, (u8 *)bh->b_data, scratch);

		ret = beamfs_seal_block(sb, (u8 *)bh->b_data);
		if (ret < 0) {
			pr_err_ratelimited("beamfs/inline: writeback_range: rs_encode_region failed: %d\n",
					   ret);
			unlock_buffer(bh);
			brelse(bh);
			break;
		}

		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)bh->b_data,
					     scratch, inode->i_ino, b);
		/*
		 * Every byte of the block has just been written, so the
		 * buffer is current whether or not it was ever read.
		 * sb_getblk hands back a buffer that is not uptodate.
		 */
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);


		/*
		 * The folio is finished from the completion of the last block
		 * of the range, not here: the write is still in flight when
		 * this returns.
		 */
		/*
		 * The folio is finished by the caller once the batch is
		 * handed over, not from a per-block completion.
		 *
		 * iomap_finish_folio_write does not claim the data are on
		 * the platter -- it says the filesystem is done touching
		 * the folio. XFS decrements it as it accumulates into an
		 * ioend, without waiting for the bio, and the bio holds
		 * block-device folios pinned by get_bh rather than the
		 * file's own, so nothing it points at can go away.
		 */

		/*
		 * The lock is held from before the decode through to the
		 * hand-over, without the release-and-retake the old
		 * completion path needed.
		 *
		 * Dropping it between the encode and the submit opened a
		 * window where another writeback on the same physical
		 * block could re-scatter and re-encode over finished
		 * codewords -- the "hot sha == cold sha mismatch"
		 * signature. Holding it throughout closes that, and the
		 * batch completion is what finally releases it.
		 */

		/*
		 * Queue rather than wait. Waiting per 4 KiB block measured at
		 * roughly 55 blocks/s on the qemu-arm64 VirtIO rig, which made
		 * any large writer appear hung in balance_dirty_pages behind a
		 * flusher that was effectively single-block-synchronous. What
		 * the block layer eventually writes is already a valid RS
		 * codeword, so resilience does not depend on the wait.
		 */
		/*
		 * Submit in both sync modes. Under iomap the folio's
		 * writeback ends from the buffer's completion, so leaving
		 * the buffer merely dirty for the flusher to pick up later
		 * means the completion never fires and sync() waits on a
		 * folio that stays in writeback forever. WB_SYNC_ALL only
		 * changes the priority hint here; the wait itself is done
		 * once for the whole range by the writepages caller.
		 */
		/*
		 * submit_bh, not write_dirty_buffer.
		 *
		 * write_dirty_buffer overwrites b_end_io with
		 * end_buffer_write_sync -- erasing the completion armed a
		 * few lines above -- and returns without submitting
		 * anything at all if the buffer is already clean. Either
		 * way beamfs_inline_wb_end_io never runs, so
		 * iomap_finish_folio_write never runs, and the folio stays
		 * in writeback for the life of the mount.
		 *
		 * xfstests generic/285 caught it: two rm processes sat in
		 * folio_wait_writeback under beamfs_evict_inode for nine
		 * hours, with no disk I/O and no hung-task report, holding
		 * the whole run behind them.
		 *
		 * Clearing the dirty bit by hand is what write_dirty_buffer
		 * did for us; submitting by hand is what keeps the
		 * completion we installed.
		 */
		if (test_clear_buffer_dirty(bh)) {
			/*
			 * submit_bh_wbc asserts b_end_io is set. Only the
			 * last block of the range carries the folio
			 * completion; the rest get the plain one, which is
			 * what write_dirty_buffer installed unconditionally
			 * and what its removal took away -- a BUG at
			 * fs/buffer.c:2701 on the first flush after mount,
			 * before the rootfs finished coming up.
			 */
			/*
			 * Set it, never inherit it.
			 *
			 * The buffer comes from the block device's cache and
			 * can carry a b_end_io from an earlier life. Only
			 * filling it in when NULL therefore submitted with
			 * whatever was left there: end_buffer_async_write in
			 * the case that killed the machine, whose first act
			 * is BUG_ON(!buffer_async_write(bh)). The flag was
			 * not set, the BUG fired inside the completion
			 * interrupt, and a BUG in interrupt context is a
			 * panic -- generic/027 took the node down every time,
			 * with no ssh and no dmesg, which is why this took so
			 * long to see.
			 *
			 * Only the block carrying the folio completion keeps
			 * its own handler; everything else gets the plain one.
			 */
			/*
			 * One request per block, and the buffer's own
			 * completion.
			 *
			 * A batched version of this lived here for a day and
			 * is gone. It grouped contiguous blocks into a
			 * single bio, which the device model says is worth
			 * a great deal: 413 us of fixed cost per request
			 * measured over nine sizes, so one request per 3824
			 * bytes caps the filesystem at 9 MB/s whatever else
			 * the code does.
			 *
			 * It could not work in this position. iomap calls
			 * writeback_range once per folio, and a 4096-byte
			 * folio holds two beamfs blocks -- so the batch
			 * never grew past two, and the ceiling stayed where
			 * it was. What it did add was a deadlock.
			 *
			 * Holding buffers locked across the rest of the loop
			 * means holding them across lookup_or_alloc_phys,
			 * which calls sb_bread and can sleep on a folio lock
			 * in the block device cache -- the same cache those
			 * buffers live in. generic/076 closes the cycle on
			 * purpose: it runs cat on the raw scratch device
			 * while fsstress writes through the filesystem, so
			 * the reader holds folio F, sync_bdevs holds F and
			 * waits for its buffers, we hold those buffers, and
			 * we ask for F. Four parties, no way out. The
			 * flusher sat there for forty minutes with io_ms
			 * unchanged across samples and PSI at 94%.
			 *
			 * The rule the batch broke is the ordinary one: a
			 * filesystem may not hold a buffer lock while
			 * sleeping on the cache that buffer belongs to.
			 * Grouping is still worth having, but it has to
			 * happen where the locks are not held -- across
			 * folios via wpc->wb_ctx, with the mapping resolved
			 * before anything is locked. That is a different
			 * piece of work and it starts from this constraint
			 * rather than discovering it.
			 */

			/*
			 * Set b_end_io, never inherit it.
			 *
			 * The buffer comes from the block device cache and
			 * can carry a handler from an earlier life. Filling
			 * it in only when NULL submitted with whatever was
			 * left there: end_buffer_async_write in the case
			 * that killed the machine, whose first act is
			 * BUG_ON(!buffer_async_write(bh)). The flag was not
			 * set, the BUG fired inside the completion
			 * interrupt, and a BUG in interrupt context is a
			 * panic -- generic/027 took the node down every
			 * time, with no ssh and no dmesg to show why.
			 */
			if (p + slice_length >= end) {
				wb = kmalloc_obj(*wb, GFP_NOFS);
				if (!wb) {
					unlock_buffer(bh);
					brelse(bh);
					ret = -ENOMEM;
					break;
				}
				wb->inode        = inode;
				wb->folio        = folio;
				wb->len          = len;
				wb->orig_private = bh->b_private;
				bh->b_private = wb;
				last_bh = bh;
			}
			get_bh(bh);
			/*
			 * The completion is an argument now. bh_end_write is
			 * what end_buffer_write_sync became; this filesystem's
			 * own completion goes in its place on the last buffer
			 * of the range, the one that finishes the folio.
			 */
			bh_submit(bh,
				  REQ_OP_WRITE |
				  ((wpc->wbc &&
				    wpc->wbc->sync_mode == WB_SYNC_ALL) ?
				   REQ_SYNC : 0),
				  (bh == last_bh) ? beamfs_inline_wb_end_io
						  : bh_end_write);
		} else {
			/*
			 * Already clean, so no completion is coming. If this
			 * was the block carrying the folio's completion, run
			 * it here rather than leaving the folio waiting.
			 */
			unlock_buffer(bh);
			if (bh == last_bh) {
				struct beamfs_inline_wb_ctx *ctx = bh->b_private;

				/* b_end_io went away; only b_private is ours to restore. */
				bh->b_private = ctx->orig_private;
				kfree(ctx);
				iomap_finish_folio_write(inode, folio, len);
				last_bh = NULL;
				folio_done = true;
			}
		}
		brelse(bh);

		p    += slice_length;
		done += slice_length;
	}

	beamfs_scratch_put(sb, scratch);

	/*
	 * On success the whole range counts as handled:
	 * iomap_writeback_range() loops while rlen remains, and a short
	 * count would bring it back for a folio already handed to the
	 * completion path. If no completion was armed, finish the folio
	 * here, because nothing else will.
	 *
	 * On error, do not. iomap_writeback_range() returns immediately
	 * without adding anything to bytes_submitted, so
	 * iomap_writeback_folio() ends the folio itself for its full size
	 * -- and a finish here as well is one too many. With one block per
	 * folio there is no counter to absorb it: iomap_finish_folio_write
	 * ends the writeback on the first call and the second acts on a
	 * folio that is no longer in it.
	 *
	 * The visible form was ENOSPC on the indirect path leaving 128
	 * fsstress processes in folio_wait_writeback under evict_inode,
	 * each holding a lock the next test waited on.
	 */
	/*
	 * The folio is done once the batch is away.
	 *
	 * On success the whole range counts as handled:
	 * iomap_writeback_range() loops while rlen remains, and a short
	 * count would bring it back for a folio already accounted for.
	 *
	 * On error, do not: iomap_writeback_range() returns without
	 * adding anything to bytes_submitted, so iomap_writeback_folio()
	 * ends the folio itself for its full size, and a second finish
	 * here would be one too many. With one block per folio there is
	 * no counter to absorb it.
	 */
	if (ret >= 0 && !last_bh && !folio_done)
		iomap_finish_folio_write(inode, folio, len);

	(void)done;
	(void)end_pos;
	return ret < 0 ? ret : (ssize_t)len;
}

static int beamfs_inline_writeback_submit(struct iomap_writepage_ctx *wpc,
					  int error)
{
	/*
	 * Nothing is batched: writeback_range hands each buffer to the block
	 * layer as it goes, so there is no context to submit here.
	 */
	(void)wpc;
	return error;
}

static const struct iomap_writeback_ops beamfs_inline_writeback_ops = {
	.writeback_range  = beamfs_inline_writeback_range,
	.writeback_submit = beamfs_inline_writeback_submit,
};

/* ------------------------------------------------------------------------- */
/* write_end (v2 INLINE)                                                     */
/*                                                                           */
/* Standard kernel pattern: flush dcache, mark folio uptodate + dirty,       */
/* update i_size if the write extended the file, then release the folio.    */
/* The actual RS encode + disk write happens later in writepages.            */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* writepages (v2 INLINE, multi-block scope)                                 */
/*                                                                           */
/* Iterate every dirty folio in the mapping via filemap_get_folios_tag.      */
/* For each folio, dispatch to beamfs_inline_writeback_folio which performs  */
/* the per-folio RMW across the 1-or-2 INLINE disk blocks the folio covers.  */
/* Cross-block-boundary writes hit two blocks; aligned writes hit one.       */
/* Block allocation (lookup_or_alloc_phys) is serialized by the per-inode    */
/* i_alloc_mutex to keep the i_direct[]/i_indirect tree consistent under     */
/* concurrent writeback of distinct folios on the same inode.                */
/* ------------------------------------------------------------------------- */
static int beamfs_inline_writepages(struct address_space *mapping,
				    struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = {
		.inode = mapping->host,
		.wbc   = wbc,
		.ops   = &beamfs_inline_writeback_ops,
	};

	return iomap_writepages(&wpc);
}

/* ------------------------------------------------------------------------- */
/* write_iter entry point                                                    */
/* ------------------------------------------------------------------------- */
static ssize_t beamfs_inline_file_write_iter(struct kiocb *iocb,
					     struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t before;
	ssize_t ret;

	/*
	 * generic_write_checks first, and it is not optional.
	 *
	 * It is what moves ki_pos to i_size when the file was opened
	 * O_APPEND, and it enforces the rlimit and the s_maxbytes bound
	 * along the way. Without it every append wrote at whatever
	 * position the descriptor happened to hold, which for a fresh
	 * open is zero: three four-byte appends left a four-byte file
	 * reading CCCC, and xfstests generic/069 reported "maybe corrupt
	 * O_APPEND" with one file holding twelve megabytes that belonged
	 * to its siblings.
	 *
	 * generic_file_write_iter calls it, which is why filesystems
	 * built on that never notice. iomap_file_buffered_write sits
	 * below it and expects the caller to have done the checks --
	 * xfs_file_write_checks exists for this, and says so in a
	 * comment: "that assigns ki_pos for O_APPEND".
	 */
	/*
	 * inode_lock, like every other buffered write in the tree.
	 *
	 * generic_file_write_iter takes it for ext2; ext4 and XFS take
	 * it by hand. Without it a truncate -- do_truncate holds
	 * inode_lock through setattr -- runs while this write is still
	 * populating the page cache of the same inode: i_size is written
	 * by both sides in no order, truncate_setsize drops folios this
	 * write is still creating, and the block tree is freed under a
	 * mapping iomap_begin has already handed out.
	 *
	 * generic/464 makes that the common case: sixteen processes doing
	 * pwrite -ftc on two hundred files chosen at random.
	 *
	 * i_alloc_mutex serialises the tree walks. It does not serialise
	 * the write against the truncate; that is what the inode lock is
	 * for.
	 */
	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	before = i_size_read(inode);

	ret = iomap_file_buffered_write(iocb, from, &beamfs_inline_iomap_ops,
					&beamfs_inline_write_ops, NULL);
	if (ret <= 0)
		goto out;

	/*
	 * iomap_write_iter() states the division of labour outright:
	 * "Update the in-memory inode size after copying the data into the
	 * page cache. It's up to the file system to write the updated size
	 * to disk." It grows i_size and raises IOMAP_F_SIZE_CHANGED; the
	 * inode does not reach the medium unless someone here says so.
	 *
	 * Returning its result directly meant a write that extended a file
	 * without a following fsync had the new size in stat and the old
	 * one on disk. xfstests generic/169: write 5 bytes, fsync, write 5
	 * more, unmount -- the file comes back 5 bytes long. The first
	 * half of that test passes only because every write there is
	 * fsynced.
	 *
	 * generic_write_sync last, as fuse does: it honours O_SYNC and
	 * O_DSYNC on the kiocb, so a caller that asked for durability gets
	 * it, and one that did not pays nothing.
	 */
	if (i_size_read(inode) != before)
		inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(inode, current_time(inode));
	mark_inode_dirty(inode);

out:
	inode_unlock(inode);
	/*
	 * Outside the lock, as ext4 does: it may wait on the device and
	 * has no business holding the inode while it does.
	 */
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

/* ------------------------------------------------------------------------- */
/* Public ops structures                                                     */
/* ------------------------------------------------------------------------- */

const struct address_space_operations beamfs_inline_aops = {
	.read_folio       = beamfs_inline_read_folio,
	.writepages       = beamfs_inline_writepages,
	/*
	 * iomap_dirty_folio, not filemap_dirty_folio.
	 *
	 * filemap's marks the folio dirty and stops there. iomap's also
	 * allocates the iomap_folio_state and calls
	 * iomap_set_range_dirty, which is what iomap_find_dirty_range
	 * later reads to decide what to write back.
	 *
	 * Without it the folio is dirty to the VFS and clean to iomap:
	 * iomap_writeback_folio calls folio_start_writeback, finds no
	 * dirty range to hand to ->writeback_range, submits nothing --
	 * and nothing ever ends the writeback it just started. The folio
	 * stays that way for the life of the mount, and the next process
	 * to touch the file waits in folio_wait_writeback forever.
	 *
	 * Found through xfstests: rm sat in truncate_inode_partial_folio
	 * under beamfs_evict_inode for nine hours while the tracing said
	 * every folio that reached writeback_range was accounted for --
	 * correctly, because the one that mattered never got there.
	 *
	 * gfs2 declares the same, which is the shortest way to see it.
	 */
	.dirty_folio      = iomap_dirty_folio,
	/* Required once the folio state is iomap's: the kernel warns at
	 * compaction time without it.
	 */
	.migrate_folio    = filemap_migrate_folio,
	.release_folio    = iomap_release_folio,
	.invalidate_folio = iomap_invalidate_folio,
};

/*
 * Same as the non-inline path: generic_file_fsync() went away with
 * sync_mapping_buffers() and i_private_list. Metadata buffer_heads
 * owned by the inode are flushed through mmb_fsync().
 */
static int beamfs_inline_fsync(struct file *file, loff_t start, loff_t end,
			       int datasync)
{
	return simple_fsync(file, start, end, datasync);
}

const struct file_operations beamfs_inline_file_operations = {
	.llseek      = generic_file_llseek,
	.read_iter   = generic_file_read_iter,
	.write_iter  = beamfs_inline_file_write_iter,
	.mmap        = generic_file_mmap,
	.fsync       = beamfs_inline_fsync,
	.splice_read = filemap_splice_read,
	/*
	 * splice_write is what copy_file_range falls back to.
	 *
	 * The VFS has no .copy_file_range here and no .remap_file_range,
	 * so vfs_copy_file_range reaches do_splice_direct -- which needs
	 * an output side. Without it the syscall returns EINVAL, and fsx
	 * stops on its first COPY operation, taking generic/075 with it.
	 *
	 * iter_file_splice_write goes through write_iter, so the RS
	 * encode and the append handling apply to spliced data exactly as
	 * they do to written data. ext2 declares the same pair, which is
	 * why it never had this gap.
	 */
	.splice_write = iter_file_splice_write,
};

/* ------------------------------------------------------------------------- */
/* Truncate support (sub-step 6 INLINE-MULTIBLOCK)                           */
/*                                                                           */
/* The allocator reaches every level: lookup_or_alloc_phys has allocation    */
/* sites for direct, indirect, double and triple indirect, so the write      */
/* path is not capped at 524 blocks. Measured 2026-08-20 on a freshly        */
/* formatted volume: a 64 MiB file, about 17500 blocks and well into double  */
/* indirect, was written and read back intact at 24 MB/s with no             */
/* EOPNOTSUPP. Capacity per file is BEAMFS_MAX_IBLOCK_TINDIRECT * 3824,      */
/* roughly 512 GiB. See BEAMFS_DINDIRECT_PTRS / BEAMFS_TINDIRECT_PTRS in     */
/* beamfs.h and the Documentation/format-v5.md section on indirect           */
/* addressing.                                                               */
/*                                                                           */
/* Truncate, however, still walks direct and single indirect only, so        */
/* freeing a file larger than 524 blocks leaves the deeper levels            */
/* allocated. That is a space leak rather than a correctness problem for     */
/* reads, and closing it is the remaining part of sub-step 6. The 64BIT +    */
/* EXTENTS feature flags remain orthogonal to multi-level indirect and       */
/* target the post-v5.0 patch series.                                        */
/* ------------------------------------------------------------------------- */

/*
 * beamfs_inline_free_blocks_from - free disk blocks at logical iblock
 *                                  >= b_first_freed.
 *
 * Mirror of beamfs_free_data_blocks (super.c) restricted to a starting
 * logical block, used for truncate-down. Walks direct[b_first_freed..N-1]
 * then the single indirect block. If b_first_freed == 0 the indirect
 * block itself is freed; otherwise individual indirect slots are zeroed
 * and the indirect block is kept.
 *
 * Caller holds inode_lock via notify_change. No allocation occurs here,
 * so i_alloc_mutex is not needed.
 */
static void beamfs_inline_free_blocks_from(struct inode *inode,
					   u64 b_first_freed)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	unsigned int              i;

	/* --- Direct blocks --- */
	for (i = b_first_freed; i < BEAMFS_DIRECT_BLOCKS; i++) {
		u64 blk = le64_to_cpu(fi->i_direct[i]);

		if (blk) {
			beamfs_free_block(sb, blk, inode);
			fi->i_direct[i] = 0;
		}
	}

	/* --- Single indirect block --- */
	if (fi->i_indirect) {
		u64                 indirect_blk = le64_to_cpu(fi->i_indirect);
		struct buffer_head *ibh = NULL;
		__le64             *ptrs;
		u64                 nptrs = BEAMFS_INDIRECT_PTRS;
		u64                 slot_first;
		u64                 j;

		/* Already held when this call created it. */
		if (!ibh)
			ibh = sb_bread(sb, indirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: truncate: failed to read indirect block %llu\n",
					   (unsigned long long)indirect_blk);
			return;
		}
		/*
		 * Verify before trusting the pointers. Under CRC this turns a
		 * flipped pointer that lands in range and on an allocated block
		 * -- the residual left open by the bounds check above -- into a
		 * clean failure instead of someone else's data. Under RS it is
		 * corrected in place and the read continues.
		 */
		if (beamfs_ind_parity_verify(sb, ibh)) {
			brelse(ibh);
			pr_err_ratelimited("beamfs/inline: truncate: indirect block %llu failed parity, subtree left allocated\n",
						   (unsigned long long)indirect_blk);
			return;
		}
		ptrs = (__le64 *)ibh->b_data;

		if (b_first_freed >= BEAMFS_DIRECT_BLOCKS)
			slot_first = b_first_freed - BEAMFS_DIRECT_BLOCKS;
		else
			slot_first = 0;

		/*
		 * Under the buffer lock, as the install side is.
		 *
		 * Zeroing a pointer is a store into a shared indirect
		 * block, no different from installing one: the flusher
		 * can be submitting that same buffer while the loop runs,
		 * and a slot cleared halfway through the write reaches
		 * the disk as neither the old value nor zero. The three
		 * install sites take the lock; the two free sites did not.
		 *
		 * beamfs_free_block sleeps -- write_bitmap_block takes
		 * lock_buffer on a bitmap block -- so the pointers are
		 * collected under the lock first and freed after it, one
		 * pass each, rather than sleeping with the buffer held.
		 */

		{
			u64 *doomed;
			u64 n_doomed = 0;

			doomed = kvmalloc_array(nptrs, sizeof(*doomed),
						GFP_NOFS);
			if (!doomed) {
				brelse(ibh);
				return;
			}

			lock_buffer(ibh);
			for (j = slot_first; j < nptrs; j++) {
				u64 blk = le64_to_cpu(ptrs[j]);

				if (blk) {
					doomed[n_doomed++] = blk;
					beamfs_tc_clear(sb, ibh->b_blocknr, (u32)j);
					ptrs[j] = 0;
				}
			}
			beamfs_ind_parity_update(sb, ibh, inode);
			unlock_buffer(ibh);
			mmb_mark_buffer_dirty(ibh,
					      &BEAMFS_I(inode)->i_metadata_bhs);
			brelse(ibh);

			for (j = 0; j < n_doomed; j++)
				beamfs_free_block(sb, doomed[j], inode);
			kvfree(doomed);
		}

		/* If we freed the entire indirect range, drop the indirect
		 * block itself.
		 */
		if (slot_first == 0) {
			beamfs_free_block(sb, indirect_blk, inode);
			fi->i_indirect = 0;
		}
	}

	/*
	 * Double and triple indirect. The allocator reaches both, so a
	 * truncate stopping at single indirect leaves every block a file
	 * held past 2 MiB allocated with nothing left pointing at it.
	 *
	 * skip counts data blocks to preserve within each subtree: zero
	 * once the truncation point is below the level, otherwise the
	 * offset into it. A subtree entirely below the point is skipped
	 * whole, which is why the walk returns its span.
	 */
	/*
	 * The truncation point goes in as an absolute logical index, and
	 * the pointer is cleared only if the walk reports the whole
	 * subtree gone. Deriving that from the truncation point instead
	 * was how a partially freed subtree ended up with a null parent
	 * pointer and its surviving blocks orphaned.
	 */
	if (fi->i_dindirect &&
	    beamfs_free_ind_range(sb, le64_to_cpu(fi->i_dindirect), 2,
				  BEAMFS_MAX_IBLOCK_INDIRECT, b_first_freed, inode))
		fi->i_dindirect = 0;

	if (fi->i_tindirect &&
	    beamfs_free_ind_range(sb, le64_to_cpu(fi->i_tindirect), 3,
				  BEAMFS_MAX_IBLOCK_DINDIRECT, b_first_freed, inode))
		fi->i_tindirect = 0;

	mark_inode_dirty(inode);
}

/*
 * beamfs_inline_zero_tail_block - zero user bytes [zero_offset .. 3824)
 *                                 in INLINE disk block b, via RMW + RS
 *                                 re-encode.
 *
 * Used when truncate-down lands inside a block: the surviving block must
 * be valid (RS-encoded) with stale tail bytes zeroed. The block is
 * decoded into scratch, the tail is memset to zero, and the block is
 * re-encoded and synced. Mirrors writeback_folio's RMW pattern.
 *
 * Caller holds inode_lock; we additionally take i_alloc_mutex to exclude
 * concurrent writeback on the same physical block.
 */
static int beamfs_inline_zero_tail_block(struct inode *inode, u64 b,
					 u32 zero_offset)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	struct buffer_head       *bh;
	u8                       *scratch;
	u64                       phys = 0;
	int                       ret;

	if (zero_offset >= BEAMFS_DATA_INLINE_BYTES)
		return 0;

	{
		u64 before = 0;

		mutex_lock(&fi->i_alloc_mutex);
		(void)beamfs_inline_lookup_phys(inode, b, &before);
		ret = beamfs_inline_lookup_or_alloc_phys(inode, b, &phys);
		/*
		 * The same gate: this one reads the block and writes it
		 * back, so a wrong address here overwrites metadata with
		 * a zeroed tail.
		 */
		if (ret == 0 &&
		    !beamfs_phys_is_sane(inode->i_sb, inode, phys, "zero_tail"))
			ret = -EUCLEAN;
		mutex_unlock(&fi->i_alloc_mutex);
		if (ret < 0)
			return ret;
		if (before && phys && before != phys)
			pr_err("beamfs/leak: zero_tail ino=%llu iblock=%llu %llu -> %llu\n",
			       (unsigned long long)inode->i_ino, (unsigned long long)b,
			       (unsigned long long)before,
			       (unsigned long long)phys);
	}
	if (phys == 0)
		return 0; /* HOLE: nothing to zero, sparse semantics */

	scratch = beamfs_scratch_get(sb);
	if (!scratch)
		return -ENOMEM;

	/* zero_tail is part of the RMW write path (truncate-induced tail
	 * clear). rmw_path=true so any flip detected on disk is journalled
	 * with RMW_NEUTRALISED, consistent with the writeback_folio path.
	 */
	bh = sb_bread(sb, phys);
	if (!bh) {
		pr_err_ratelimited("beamfs/inline: zero_tail: sb_bread phys=%llu failed\n",
				   (unsigned long long)phys);
		beamfs_scratch_put(sb, scratch);
		return -EIO;
	}
	/*
	 * Lock bh for the full RMW transit (same rationale as
	 * writeback_folio: serialise against concurrent writeback on the
	 * same phys via the block-device page cache).
	 */
	lock_buffer(bh);

	ret = beamfs_inline_decode_block_into_buf(sb, bh, phys, inode, b,
						  scratch, 0,
						  BEAMFS_DATA_INLINE_BYTES,
						  true);
	if (ret < 0) {
		unlock_buffer(bh);
		brelse(bh);
		beamfs_scratch_put(sb, scratch);
		return ret;
	}

	memset(scratch + zero_offset, 0,
	       BEAMFS_DATA_INLINE_BYTES - zero_offset);

	beamfs_lay_payload(sb, (u8 *)bh->b_data, scratch);

	ret = beamfs_seal_block(sb, (u8 *)bh->b_data);
	if (ret < 0) {
		pr_err_ratelimited("beamfs/inline: zero_tail: rs_encode_region failed: %d\n",
				   ret);
		unlock_buffer(bh);
		brelse(bh);
		beamfs_scratch_put(sb, scratch);
		return ret;
	}

	beamfs_inline_stamp_tail_pad(BEAMFS_SB(inode->i_sb),
				     (u8 *)bh->b_data, scratch,
				     (unsigned long long)inode->i_ino, b);

	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	ret = sync_dirty_buffer(bh);
	brelse(bh);
	beamfs_scratch_put(sb, scratch);
	return ret;
}

/*
 * beamfs_inline_setattr - VFS setattr hook for INLINE inodes.
 *
 * Handles ATTR_SIZE (truncate up and down). Truncate-up extends i_size
 * sparsely (HOLE handling in read_folio returns zero for unallocated
 * blocks). Truncate-down frees disk blocks beyond the new size and
 * zeros the partial trailing bytes in the surviving block.
 *
 * Other attribute changes (mode, owner, times) are forwarded to
 * setattr_copy + mark_inode_dirty.
 */
static int beamfs_inline_setattr(struct mnt_idmap *idmap,
				 struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int           ret;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	if (attr->ia_valid & ATTR_SIZE) {
		loff_t old_size = i_size_read(inode);
		loff_t new_size = attr->ia_size;

		ret = inode_newsize_ok(inode, new_size);
		if (ret)
			return ret;

		if (new_size < old_size) {
			u64 b_first_freed;
			u32 tail_off;

			/* Drop pagecache beyond new_size before freeing
			 * the underlying disk blocks.
			 */
			truncate_setsize(inode, new_size);

			/* Logical block index of the first block to be
			 * fully freed: ceil(new_size / 3824).
			 */
			b_first_freed = (new_size +
					 beamfs_block_payload(inode->i_sb) - 1)
					/ beamfs_block_payload(inode->i_sb);

			/* If new_size is not block-aligned, the surviving
			 * last block has stale user bytes beyond new_size.
			 * Zero them via RMW + RS re-encode.
			 */
			tail_off = (u32)(new_size %
					 beamfs_block_payload(inode->i_sb));
			if (tail_off != 0 && new_size > 0) {
				u64 b_last_kept = new_size /
						  BEAMFS_DATA_INLINE_BYTES;

				ret = beamfs_inline_zero_tail_block(inode,
								    b_last_kept,
								    tail_off);
				if (ret)
					return ret;
			}

			/*
			 * The same mutex the allocator takes.
			 *
			 * Truncate walks the same indirection tree that
			 * lookup_or_alloc_phys writes into, and took no
			 * lock at all. The trace caught it: pid 3212
			 * freeing block 0xe09 at 196.057356 between pid
			 * 3112 allocating it at 196.042955 and allocating
			 * it again at 196.058203 -- one writer's pointer
			 * overwritten by another's, and the block left
			 * marked used with nothing referencing it.
			 *
			 * generic/464 runs sixteen processes doing
			 * pwrite -ftc against two hundred files chosen at
			 * random, so a truncate on one file races an
			 * allocation on another constantly. The leak
			 * varied from 1 to 734 blocks between runs, which
			 * is what a race looks like when you count it.
			 */
			mutex_lock(&BEAMFS_I(inode)->i_alloc_mutex);
			beamfs_inline_free_blocks_from(inode, b_first_freed);
			mutex_unlock(&BEAMFS_I(inode)->i_alloc_mutex);
		} else if (new_size > old_size) {
			/* Sparse extension: just adjust i_size. read_folio
			 * returns zero for unallocated (HOLE) blocks.
			 */
			truncate_setsize(inode, new_size);
		}

		/*
		 * A truncate changes the file, so it changes mtime, and it
		 * changes the inode, so it changes ctime.
		 *
		 * setattr_copy below only carries the times the VFS asked
		 * for, and truncate asks for none: the filesystem is
		 * expected to set them itself, the way it does for a write.
		 * generic/313 reports all four cases -- ctime and mtime,
		 * shrinking and growing -- and says so in as many words.
		 *
		 * Both are set even when the size did not actually move,
		 * because setattr_prepare has already accepted the request
		 * and POSIX makes no exception for a truncate to the
		 * current length.
		 */
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	}

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

/*
 * fiemap for the INLINE scheme.
 *
 * beamfs_fiemap in file.c calls iomap_fiemap with beamfs_iomap_ops --
 * the legacy scheme=5 map, where a logical block is 4096 bytes and disk
 * offset is file offset. Under INLINE a logical block is 3824 bytes
 * inside a 4096-byte physical one, so that map describes a geometry
 * this file does not have: generic/473 saw data starting at block 136
 * where it had written at 128, a drift of 272 bytes per block.
 *
 * What this reports is one extent per block, and that is not a
 * limitation of the code. fiemap merges two extents when
 * addr + length == next addr; here addr advances by 4096 while length
 * is 3824, so the test never holds and never can. The mapping from
 * file offset to disk offset is not affine, which is the price of
 * putting each block's parity inside the block: 272 bytes of every
 * 4096 belong to the code that protects the other 3824.
 *
 * So generic/473 still fails, and it fails for a reason worth stating
 * rather than hiding: a tool asking where a file lives now gets the
 * truth, block by block, instead of confident nonsense.
 */
static int beamfs_inline_fiemap(struct inode *inode,
				struct fiemap_extent_info *fieinfo,
				u64 start, u64 len)
{
	int ret;
	loff_t i_size;

	inode_lock(inode);
	i_size = i_size_read(inode);
	/* iomap_fiemap rejects len == 0; keep the call valid for an
	 * empty file, where it returns no extents.
	 */
	if (i_size == 0)
		i_size = 1;
	len = min_t(u64, len, i_size);
	ret = iomap_fiemap(inode, fieinfo, start, len,
			   &beamfs_inline_iomap_ops);
	inode_unlock(inode);
	return ret;
}

const struct inode_operations beamfs_inline_inode_operations = {
	.getattr        = simple_getattr,
	.setattr        = beamfs_inline_setattr,
	.fiemap         = beamfs_inline_fiemap,
};
