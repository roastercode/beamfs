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
	struct buffer_head       *ibh;
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
		struct buffer_head *l1bh;

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
		struct buffer_head *l1bh, *l2bh;

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
static int beamfs_inline_lookup_or_alloc_phys(struct inode *inode,
					      u64 iblock_logical,
					      u64 *phys_out)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block       *sb = inode->i_sb;
	struct buffer_head       *ibh;
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
			pr_err_ratelimited("beamfs/inline: no free blocks (direct)\n");
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
		beamfs_ind_parity_update(sb, dbh);
		mark_buffer_dirty(dbh);
		brelse(dbh);

		fi->i_direct[iblock_logical] = cpu_to_le64(new_block);
		mark_inode_dirty(inode);

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
				pr_err_ratelimited("beamfs/inline: no free blocks (indirect)\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, indirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, indirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			unlock_buffer(ibh);
			beamfs_ind_parity_update(sb, ibh);
			mark_buffer_dirty(ibh);
			brelse(ibh);

			fi->i_indirect = cpu_to_le64(indirect_blk);
			mark_inode_dirty(inode);
		}

		/* Read indirect to look up / install the slot. */
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
			pr_err_ratelimited("beamfs/inline: no free blocks (data)\n");
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
		beamfs_ind_parity_update(sb, dbh);
		mark_buffer_dirty(dbh);
		brelse(dbh);

		ptrs[indirect_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, ibh);
		mark_buffer_dirty(ibh);
		brelse(ibh);

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
		struct buffer_head *l1bh;

		didx = iblock_logical - BEAMFS_MAX_IBLOCK_INDIRECT;
		l1_slot = didx / BEAMFS_INDIRECT_PTRS;
		l2_slot = didx % BEAMFS_INDIRECT_PTRS;

		/* --- Stage 1: dindirect block --- */
		dindirect_blk = le64_to_cpu(fi->i_dindirect);
		if (!dindirect_blk) {
			dindirect_blk = beamfs_alloc_block(sb, inode);
			if (!dindirect_blk) {
				pr_err_ratelimited("beamfs/inline: no free blocks (dindirect)\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, dindirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, dindirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			unlock_buffer(ibh);
			beamfs_ind_parity_update(sb, ibh);
			mark_buffer_dirty(ibh);
			brelse(ibh);
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
				pr_err_ratelimited("beamfs/inline: no free blocks (L1 indirect)\n");
				return -ENOSPC;
			}
			l1bh = sb_getblk(sb, l1_blk);
			if (!l1bh) {
				beamfs_free_block(sb, l1_blk, inode);
				brelse(ibh);
				return -EIO;
			}
			lock_buffer(l1bh);
			memset(l1bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l1bh);
			unlock_buffer(l1bh);
			beamfs_ind_parity_update(sb, l1bh);
			mark_buffer_dirty(l1bh);
			brelse(l1bh);
			ptrs[l1_slot] = cpu_to_le64(l1_blk);
			beamfs_ind_parity_update(sb, ibh);
			mark_buffer_dirty(ibh);
		}
		brelse(ibh);

		/* --- Stage 3: data block --- */
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
			pr_err_ratelimited("beamfs/inline: no free blocks (dindirect data)\n");
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
		beamfs_ind_parity_update(sb, dbh);
		mark_buffer_dirty(dbh);
		brelse(dbh);

		ptrs[l2_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, l1bh);
		mark_buffer_dirty(l1bh);
		brelse(l1bh);

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
		struct buffer_head *l1bh, *l2bh;

		tidx = iblock_logical - BEAMFS_MAX_IBLOCK_DINDIRECT;
		l1_slot = tidx / (BEAMFS_INDIRECT_PTRS * BEAMFS_INDIRECT_PTRS);
		l2_slot = (tidx / BEAMFS_INDIRECT_PTRS) % BEAMFS_INDIRECT_PTRS;
		l3_slot = tidx % BEAMFS_INDIRECT_PTRS;

		/* --- Stage 1: tindirect block --- */
		tindirect_blk = le64_to_cpu(fi->i_tindirect);
		if (!tindirect_blk) {
			tindirect_blk = beamfs_alloc_block(sb, inode);
			if (!tindirect_blk) {
				pr_err_ratelimited("beamfs/inline: no free blocks (tindirect)\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, tindirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, tindirect_blk, inode);
				return -EIO;
			}
			lock_buffer(ibh);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			unlock_buffer(ibh);
			beamfs_ind_parity_update(sb, ibh);
			mark_buffer_dirty(ibh);
			brelse(ibh);
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
				pr_err_ratelimited("beamfs/inline: no free blocks (tindirect L1)\n");
				return -ENOSPC;
			}
			l1bh = sb_getblk(sb, l1_blk);
			if (!l1bh) {
				beamfs_free_block(sb, l1_blk, inode);
				brelse(ibh);
				return -EIO;
			}
			lock_buffer(l1bh);
			memset(l1bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l1bh);
			unlock_buffer(l1bh);
			beamfs_ind_parity_update(sb, l1bh);
			mark_buffer_dirty(l1bh);
			brelse(l1bh);
			ptrs[l1_slot] = cpu_to_le64(l1_blk);
			beamfs_ind_parity_update(sb, ibh);
			mark_buffer_dirty(ibh);
		}
		brelse(ibh);

		/* --- Stage 3: level-2 indirect block --- */
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
				pr_err_ratelimited("beamfs/inline: no free blocks (tindirect L2)\n");
				return -ENOSPC;
			}
			l2bh = sb_getblk(sb, l2_blk);
			if (!l2bh) {
				beamfs_free_block(sb, l2_blk, inode);
				brelse(l1bh);
				return -EIO;
			}
			lock_buffer(l2bh);
			memset(l2bh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(l2bh);
			unlock_buffer(l2bh);
			beamfs_ind_parity_update(sb, l2bh);
			mark_buffer_dirty(l2bh);
			brelse(l2bh);
			ptrs[l2_slot] = cpu_to_le64(l2_blk);
			beamfs_ind_parity_update(sb, l1bh);
			mark_buffer_dirty(l1bh);
		}
		brelse(l1bh);

		/* --- Stage 4: data block --- */
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
			pr_err_ratelimited("beamfs/inline: no free blocks (tindirect data)\n");
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
		beamfs_ind_parity_update(sb, dbh);
		mark_buffer_dirty(dbh);
		brelse(dbh);

		ptrs[l3_slot] = cpu_to_le64(new_block);
		beamfs_ind_parity_update(sb, l2bh);
		mark_buffer_dirty(l2bh);
		brelse(l2bh);

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
	tmp = kmalloc(BEAMFS_BLOCK_SIZE, GFP_NOFS);
	if (!tmp)
		return -ENOMEM;
	memcpy(tmp, bh->b_data, BEAMFS_BLOCK_SIZE);

	beamfs_rs_decode_region(
		tmp, BEAMFS_SUBBLOCK_TOTAL,
		tmp + BEAMFS_SUBBLOCK_DATA, BEAMFS_SUBBLOCK_TOTAL,
		BEAMFS_SUBBLOCK_DATA, BEAMFS_DATA_INLINE_SUBBLOCKS,
		rs_results,
		rs_positions,
		BEAMFS_RS_PARITY / 2);

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
			beamfs_log_rs_event_flagged(sb,
				(u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
				NULL, 0,
				BEAMFS_SUBBLOCK_DATA,
				xflags);
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
			beamfs_log_rs_event_flagged(sb,
				(u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
				pos, np,
				BEAMFS_SUBBLOCK_DATA,
				xflags);
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

		for (sb_idx = sb_first; sb_idx <= sb_last; sb_idx++) {
			u32 sb_user_start = sb_idx * BEAMFS_SUBBLOCK_DATA;
			u32 sb_user_end   = sb_user_start + BEAMFS_SUBBLOCK_DATA;
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

	kfree(tmp);
	return 0;

out_brelse:
	kfree(tmp);
	return ret;
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
	u64 b = (u64)pos / BEAMFS_DATA_INLINE_BYTES;
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
	if (pos >= i_size) {
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
	iomap->length = length;

	ret = beamfs_inline_lookup_phys(inode, b, &phys);
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

	(void)flags;
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
		u64    b            = pos / BEAMFS_DATA_INLINE_BYTES;
		u32    slice_offset = (u32)(pos % BEAMFS_DATA_INLINE_BYTES);
		u32    slice_length = (u32)min_t(u64, end - pos,
						 BEAMFS_DATA_INLINE_BYTES -
						 slice_offset);
		size_t folio_off    = offset_in_folio(folio, pos);
		struct buffer_head *bh;
		u64    phys = 0;
		u8    *dst;

		ret = beamfs_inline_lookup_phys(inode, b, &phys);
		if (ret < 0)
			break;

		if (phys == 0) {
			folio_zero_range(folio, folio_off, slice_length);
			pos  += slice_length;
			done += slice_length;
			continue;
		}

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
		u64    b            = p / BEAMFS_DATA_INLINE_BYTES;
		u32    slice_offset = (u32)(p % BEAMFS_DATA_INLINE_BYTES);
		u32    slice_length = (u32)min_t(u64, end - p,
						 BEAMFS_DATA_INLINE_BYTES -
						 slice_offset);
		size_t folio_off    = offset_in_folio(folio, p);
		struct buffer_head *bh;
		u64    phys = 0;
		u8    *dst;

		ret = beamfs_inline_lookup_phys(inode, b, &phys);
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
 * Writeback completion for one INLINE block.
 *
 * iomap_writeback_folio() ends the folio itself only when nothing was
 * submitted; past that it waits for the filesystem to report completion.
 * mark_buffer_dirty() and write_dirty_buffer() are asynchronous, so the
 * folio cannot be finished when writeback_range returns -- the I/O is
 * still in flight, and sync() would then wait on a completion that never
 * arrives. The buffer's end_io callback is where the folio is released,
 * once per block, with the folio held until the last one lands.
 */
struct beamfs_inline_wb_ctx {
	struct inode  *inode;
	struct folio  *folio;
	size_t         len;
	bh_end_io_t   *orig_end_io;
	void          *orig_private;
};

static void beamfs_inline_wb_end_io(struct buffer_head *bh, int uptodate)
{
	struct beamfs_inline_wb_ctx *wb = bh->b_private;
	struct inode *inode = wb->inode;
	struct folio *folio = wb->folio;
	size_t len = wb->len;

	bh->b_end_io = wb->orig_end_io;
	bh->b_private = wb->orig_private;
	kfree(wb);

	if (!uptodate)
		mapping_set_error(inode->i_mapping, -EIO);

	end_buffer_write_sync(bh, uptodate);
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
	struct beamfs_inline_wb_ctx *wb = NULL;
	struct buffer_head *last_bh = NULL;
	bool     folio_done = false;
	ssize_t  done = 0;
	int      ret  = 0;

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

	scratch = kmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
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
		u64    b            = p / BEAMFS_DATA_INLINE_BYTES;
		u32    slice_offset = (u32)(p % BEAMFS_DATA_INLINE_BYTES);
		u32    slice_length = (u32)min_t(u64, end - p,
						 BEAMFS_DATA_INLINE_BYTES -
						 slice_offset);
		size_t folio_off    = offset_in_folio(folio, p);
		struct buffer_head *bh;
		unsigned int sb_idx;
		u64    phys = 0;
		u8    *src;

		mutex_lock(&fi->i_alloc_mutex);
		ret = beamfs_inline_lookup_or_alloc_phys(inode, b, &phys);
		mutex_unlock(&fi->i_alloc_mutex);
		if (ret < 0)
			break;

		bh = sb_bread(sb, phys);
		if (!bh) {
			pr_err_ratelimited("beamfs/inline: writeback_range: sb_bread phys=%llu failed\n",
					   (unsigned long long)phys);
			ret = -EIO;
			break;
		}

		lock_buffer(bh);

		ret = beamfs_inline_decode_block_into_buf(sb, bh, phys, inode, b,
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

		for (sb_idx = 0; sb_idx < BEAMFS_DATA_INLINE_SUBBLOCKS; sb_idx++)
			memcpy((u8 *)bh->b_data +
			       (size_t)sb_idx * BEAMFS_SUBBLOCK_TOTAL,
			       scratch + (size_t)sb_idx * BEAMFS_SUBBLOCK_DATA,
			       BEAMFS_SUBBLOCK_DATA);

		ret = beamfs_rs_encode_region((u8 *)bh->b_data,
					      BEAMFS_SUBBLOCK_TOTAL,
					      (u8 *)bh->b_data +
					      BEAMFS_SUBBLOCK_DATA,
					      BEAMFS_SUBBLOCK_TOTAL,
					      BEAMFS_SUBBLOCK_DATA,
					      BEAMFS_DATA_INLINE_SUBBLOCKS);
		if (ret < 0) {
			pr_err_ratelimited("beamfs/inline: writeback_range: rs_encode_region failed: %d\n",
					   ret);
			unlock_buffer(bh);
			brelse(bh);
			break;
		}

		beamfs_inline_stamp_tail_pad(BEAMFS_SB(sb), (u8 *)bh->b_data,
					     scratch, inode->i_ino, b);
		mark_buffer_dirty(bh);

		/*
		 * The folio is finished from the completion of the last block
		 * of the range, not here: the write is still in flight when
		 * this returns.
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
			wb->orig_end_io  = bh->b_end_io;
			wb->orig_private = bh->b_private;
			bh->b_private = wb;
			bh->b_end_io  = beamfs_inline_wb_end_io;
			last_bh = bh;
		}

		unlock_buffer(bh);

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
		lock_buffer(bh);
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
			if (!bh->b_end_io)
				bh->b_end_io = end_buffer_write_sync;
			get_bh(bh);
			submit_bh(REQ_OP_WRITE |
				  ((wpc->wbc &&
				    wpc->wbc->sync_mode == WB_SYNC_ALL) ?
				   REQ_SYNC : 0), bh);
		} else {
			/*
			 * Already clean, so no completion is coming. If this
			 * was the block carrying the folio's completion, run
			 * it here rather than leaving the folio waiting.
			 */
			unlock_buffer(bh);
			if (bh == last_bh) {
				struct beamfs_inline_wb_ctx *ctx = bh->b_private;

				bh->b_end_io  = ctx->orig_end_io;
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

	kfree(scratch);

	/*
	 * The whole range counts as handled: iomap_writeback_range() loops
	 * while rlen remains, and a short count would bring it back for a
	 * folio already handed to the completion path.
	 *
	 * If no completion was armed -- an error before the last block --
	 * finish the folio here, otherwise nothing ever would and sync()
	 * would wait on it forever.
	 */
	if (!last_bh && !folio_done)
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
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		return ret;

	before = i_size_read(inode);

	ret = iomap_file_buffered_write(iocb, from, &beamfs_inline_iomap_ops,
					&beamfs_inline_write_ops, NULL);
	if (ret <= 0)
		return ret;

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

	return generic_write_sync(iocb, ret);
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
	struct inode *inode = file->f_mapping->host;

	return mmb_fsync(file, &BEAMFS_I(inode)->i_metadata_bhs,
			 start, end, datasync != 0);
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
		struct buffer_head *ibh;
		__le64             *ptrs;
		u64                 nptrs = BEAMFS_INDIRECT_PTRS;
		u64                 slot_first;
		u64                 j;

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

		for (j = slot_first; j < nptrs; j++) {
			u64 blk = le64_to_cpu(ptrs[j]);

			if (blk) {
				beamfs_free_block(sb, blk, inode);
				ptrs[j] = 0;
			}
		}

		beamfs_ind_parity_update(sb, ibh);
		mark_buffer_dirty(ibh);
		brelse(ibh);

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
				  BEAMFS_MAX_IBLOCK_INDIRECT, b_first_freed))
		fi->i_dindirect = 0;

	if (fi->i_tindirect &&
	    beamfs_free_ind_range(sb, le64_to_cpu(fi->i_tindirect), 3,
				  BEAMFS_MAX_IBLOCK_DINDIRECT, b_first_freed))
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
	unsigned int              sb_idx;
	int                       ret;

	if (zero_offset >= BEAMFS_DATA_INLINE_BYTES)
		return 0;

	mutex_lock(&fi->i_alloc_mutex);
	ret = beamfs_inline_lookup_or_alloc_phys(inode, b, &phys);
	mutex_unlock(&fi->i_alloc_mutex);
	if (ret < 0)
		return ret;
	if (phys == 0)
		return 0; /* HOLE: nothing to zero, sparse semantics */

	scratch = kmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
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
		kfree(scratch);
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
		kfree(scratch);
		return ret;
	}

	memset(scratch + zero_offset, 0,
	       BEAMFS_DATA_INLINE_BYTES - zero_offset);

	for (sb_idx = 0; sb_idx < BEAMFS_DATA_INLINE_SUBBLOCKS; sb_idx++) {
		memcpy((u8 *)bh->b_data + (size_t)sb_idx * BEAMFS_SUBBLOCK_TOTAL,
		       scratch + (size_t)sb_idx * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
	}

	ret = beamfs_rs_encode_region(
		(u8 *)bh->b_data, BEAMFS_SUBBLOCK_TOTAL,
		(u8 *)bh->b_data + BEAMFS_SUBBLOCK_DATA, BEAMFS_SUBBLOCK_TOTAL,
		BEAMFS_SUBBLOCK_DATA, BEAMFS_DATA_INLINE_SUBBLOCKS);
	if (ret < 0) {
		pr_err_ratelimited("beamfs/inline: zero_tail: rs_encode_region failed: %d\n",
				   ret);
		unlock_buffer(bh);
		brelse(bh);
		kfree(scratch);
		return ret;
	}

	beamfs_inline_stamp_tail_pad(BEAMFS_SB(inode->i_sb),
				     (u8 *)bh->b_data, scratch,
				     (unsigned long long)inode->i_ino, b);

	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	ret = sync_dirty_buffer(bh);
	brelse(bh);
	kfree(scratch);
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
			b_first_freed = (new_size + BEAMFS_DATA_INLINE_BYTES - 1)
					/ BEAMFS_DATA_INLINE_BYTES;

			/* If new_size is not block-aligned, the surviving
			 * last block has stale user bytes beyond new_size.
			 * Zero them via RMW + RS re-encode.
			 */
			tail_off = (u32)(new_size % BEAMFS_DATA_INLINE_BYTES);
			if (tail_off != 0 && new_size > 0) {
				u64 b_last_kept = new_size /
						  BEAMFS_DATA_INLINE_BYTES;

				ret = beamfs_inline_zero_tail_block(inode,
								    b_last_kept,
								    tail_off);
				if (ret)
					return ret;
			}

			beamfs_inline_free_blocks_from(inode, b_first_freed);
		} else if (new_size > old_size) {
			/* Sparse extension: just adjust i_size. read_folio
			 * returns zero for unallocated (HOLE) blocks.
			 */
			truncate_setsize(inode, new_size);
		}
	}

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

const struct inode_operations beamfs_inline_inode_operations = {
	.getattr        = simple_getattr,
	.setattr        = beamfs_inline_setattr,
	.fiemap         = beamfs_fiemap,  /* S2.2: shared with scheme=5, declared in file.c */
};
