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
#include "beamfs.h"

/* ------------------------------------------------------------------------- */
/* Forward declarations of v2 ops (stubs, populated in subsequent stages)    */
/* ------------------------------------------------------------------------- */

static int     beamfs_inline_read_folio(struct file *file,
					struct folio *folio);
static int     beamfs_inline_writepages(struct address_space *mapping,
					struct writeback_control *wbc);
static void    beamfs_inline_readahead(struct readahead_control *rac);
static int     beamfs_inline_write_begin(const struct kiocb *iocb,
					 struct address_space *mapping,
					 loff_t pos, unsigned int len,
					 struct folio **foliop, void **fsdata);
static int     beamfs_inline_write_end(const struct kiocb *iocb,
				       struct address_space *mapping,
				       loff_t pos, unsigned int len,
				       unsigned int copied,
				       struct folio *folio, void *fsdata);
static ssize_t beamfs_inline_file_write_iter(struct kiocb *iocb,
					     struct iov_iter *from);
static int     beamfs_inline_setattr(struct mnt_idmap *idmap,
				    struct dentry *dentry,
				    struct iattr *attr);
static void    beamfs_inline_free_blocks_from(struct inode *inode,
					      u64 b_first_freed);
static int     beamfs_inline_zero_tail_block(struct inode *inode,
					     u64 b, u32 zero_offset);

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

	if (!phys_out)
		return -EINVAL;

	*phys_out = 0;

	if (iblock_logical < BEAMFS_DIRECT_BLOCKS) {
		u64 dphys = le64_to_cpu(fi->i_direct[iblock_logical]);

		if (dphys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (dphys < sbi->s_data_start ||
			    dphys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted direct pointer ino=%lu iblock=%llu phys=%llu (out of [%lu, %lu))\n",
						   inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)dphys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
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

		ibh = sb_bread(sb, indirect_blk);
		if (!ibh) {
			pr_err_ratelimited("beamfs/inline: failed to read indirect block %llu\n",
					   (unsigned long long)indirect_blk);
			return -EIO;
		}
		ptrs = (__le64 *)ibh->b_data;
		phys = le64_to_cpu(ptrs[indirect_slot]);
		brelse(ibh);

		if (phys != 0) {
			struct beamfs_sb_info *sbi = BEAMFS_SB(sb);

			if (phys < sbi->s_data_start ||
			    phys >= sbi->s_data_start + sbi->s_nblocks) {
				pr_err_ratelimited("beamfs/inline: corrupted indirect pointer ino=%lu iblock=%llu slot=%llu phys=%llu (out of [%lu, %lu))\n",
						   inode->i_ino,
						   (unsigned long long)iblock_logical,
						   (unsigned long long)indirect_slot,
						   (unsigned long long)phys,
						   sbi->s_data_start,
						   sbi->s_data_start + sbi->s_nblocks);
				return -EUCLEAN;
			}
		}
		*phys_out = phys;
		return 0;
	}

	pr_err_ratelimited("beamfs/inline: iblock %llu beyond v1 indirect capacity\n",
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
		new_block = beamfs_alloc_block(sb);
		if (!new_block) {
			pr_err_ratelimited("beamfs/inline: no free blocks (direct)\n");
			return -ENOSPC;
		}
		/* Zero-init the freshly allocated data block on disk. */
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
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
			indirect_blk = beamfs_alloc_block(sb);
			if (!indirect_blk) {
				pr_err_ratelimited("beamfs/inline: no free blocks (indirect)\n");
				return -ENOSPC;
			}
			ibh = sb_getblk(sb, indirect_blk);
			if (!ibh) {
				beamfs_free_block(sb, indirect_blk);
				return -EIO;
			}
			lock_buffer(ibh);
			memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
			set_buffer_uptodate(ibh);
			unlock_buffer(ibh);
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
		ptrs = (__le64 *)ibh->b_data;
		phys = le64_to_cpu(ptrs[indirect_slot]);

		if (phys) {
			brelse(ibh);
			*phys_out = phys;
			return 0;
		}

		/* Allocate data block and zero-init. */
		new_block = beamfs_alloc_block(sb);
		if (!new_block) {
			brelse(ibh);
			pr_err_ratelimited("beamfs/inline: no free blocks (data)\n");
			return -ENOSPC;
		}
		dbh = sb_getblk(sb, new_block);
		if (!dbh) {
			beamfs_free_block(sb, new_block);
			brelse(ibh);
			return -EIO;
		}
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		mark_buffer_dirty(dbh);
		brelse(dbh);

		ptrs[indirect_slot] = cpu_to_le64(new_block);
		mark_buffer_dirty(ibh);
		brelse(ibh);

		*phys_out = new_block;
		return 0;
	}

	pr_err_ratelimited("beamfs/inline: iblock %llu beyond v1 indirect capacity (write)\n",
			  (unsigned long long)iblock_logical);
	return -EOPNOTSUPP;
}

/* ------------------------------------------------------------------------- */
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
					       u64 phys,
					       struct inode *inode,
					       u64 iblock_logical_for_log,
					       u8 *dst_buf,
					       u32 slice_offset,
					       u32 slice_length)
{
	struct buffer_head *bh = NULL;
	int                 rs_results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	int                 rs_positions[BEAMFS_DATA_INLINE_SUBBLOCKS *
				      (BEAMFS_RS_PARITY / 2)];
	bool                corrected = false;
	bool                uncorrectable = false;
	unsigned int        i;
	int                 ret = 0;

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

	bh = sb_bread(sb, phys);
	if (!bh) {
		pr_err_ratelimited("beamfs/inline: sb_bread failed phys=%llu\n",
				   (unsigned long long)phys);
		return -EIO;
	}

	/*
	 * Decode 16 RS(255,239) shortened subblocks in place. Same layout
	 * as the bitmap path in alloc.c: data and parity are interleaved
	 * with stride BEAMFS_SUBBLOCK_TOTAL (255), parity offset 239.
	 */
	beamfs_rs_decode_region(
		(u8 *)bh->b_data, BEAMFS_SUBBLOCK_TOTAL,
		(u8 *)bh->b_data + BEAMFS_SUBBLOCK_DATA, BEAMFS_SUBBLOCK_TOTAL,
		BEAMFS_SUBBLOCK_DATA, BEAMFS_DATA_INLINE_SUBBLOCKS,
		rs_results,
		rs_positions,
		BEAMFS_RS_PARITY / 2);

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		int rc = rs_results[i];

		if (rc < 0) {
			/*
			 * Journal the uncorrectable event before raising the
			 * error: forensic record takes priority over the alert.
			 * See Documentation/format-v4.md section 6.5.
			 */
			beamfs_log_rs_event(sb,
				(u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
				NULL, 0,
				BEAMFS_SUBBLOCK_DATA);
			pr_err_ratelimited("beamfs/inline: ino=%lu iblock=%llu subblock=%u uncorrectable\n",
					   inode->i_ino,
					   (unsigned long long)iblock_logical_for_log,
					   i);
			uncorrectable = true;
		} else if (rc > 0) {
			unsigned int np = (unsigned int)rc;
			int *pos = rs_positions +
				   (size_t)i * (BEAMFS_RS_PARITY / 2);

			if (np > BEAMFS_RS_PARITY / 2)
				np = BEAMFS_RS_PARITY / 2;
			pr_warn_ratelimited("beamfs/inline: ino=%lu iblock=%llu subblock=%u: %d symbol(s) corrected\n",
					    inode->i_ino,
					    (unsigned long long)iblock_logical_for_log,
					    i, rc);
			beamfs_log_rs_event(sb,
				(u64)phys * BEAMFS_DATA_INLINE_SUBBLOCKS + i,
				pos, np,
				BEAMFS_SUBBLOCK_DATA);
			corrected = true;
		}
	}

	if (uncorrectable) {
		ret = -EIO;
		goto out_brelse;
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
			       (u8 *)bh->b_data
				+ (size_t)sb_idx * BEAMFS_SUBBLOCK_TOTAL
				+ from_in_sb,
			       copy_len);
			dst_off += copy_len;
		}
	}

	/*
	 * Durable autonomic repair: if RS corrected any subblock, write
	 * the repaired disk block back synchronously so the on-disk image
	 * is healed before the next read. Same pattern as the bitmap
	 * recovery path in alloc.c.
	 */
	if (corrected) {
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	}

	brelse(bh);
	return 0;

out_brelse:
	brelse(bh);
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
static int beamfs_inline_folio_coverage(struct inode *inode,
					pgoff_t folio_index,
					u64 *out_b_first,
					u32 *out_k_first,
					u64 *out_b_last,
					u32 *out_len_in_b_last,
					u32 *out_folio_user_bytes)
{
	loff_t i_size = i_size_read(inode);
	u64    folio_start_byte = (u64)folio_index << PAGE_SHIFT;
	u64    folio_end_byte;     /* exclusive */
	u64    b_first, b_last;
	u32    k_first, fub, lbl;

	if (folio_start_byte >= (u64)i_size)
		return -ERANGE;

	folio_end_byte = folio_start_byte + PAGE_SIZE;
	if (folio_end_byte > (u64)i_size)
		folio_end_byte = (u64)i_size;

	b_first = folio_start_byte / BEAMFS_DATA_INLINE_BYTES;
	k_first = (u32)(folio_start_byte % BEAMFS_DATA_INLINE_BYTES);
	b_last  = (folio_end_byte - 1) / BEAMFS_DATA_INLINE_BYTES;

	fub = (u32)(folio_end_byte - folio_start_byte);

	/*
	 * lbl (len in b_last) is the number of user bytes the folio occupies
	 * within b_last. For bi-block (b_last == b_first + 1), this is
	 * fub - (INLINE_BYTES - k_first). For tri-block (b_last == b_first + 2,
	 * which occurs when k_first > 2*INLINE_BYTES - PAGE_SIZE = 3552),
	 * the b_first slice is (INLINE_BYTES - k_first), the intermediate is
	 * INLINE_BYTES full, and lbl is fub - (INLINE_BYTES - k_first) - INLINE_BYTES.
	 * General formula: lbl = fub - (INLINE_BYTES - k_first)
	 *                            - (b_last - b_first - 1) * INLINE_BYTES
	 */
	if (b_last == b_first)
		lbl = fub;
	else
		lbl = fub - (BEAMFS_DATA_INLINE_BYTES - k_first)
			  - (u32)(b_last - b_first - 1) * BEAMFS_DATA_INLINE_BYTES;

	*out_b_first         = b_first;
	*out_k_first         = k_first;
	*out_b_last          = b_last;
	*out_len_in_b_last   = lbl;
	*out_folio_user_bytes = fub;

	return 0;
}

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
static int beamfs_inline_read_folio(struct file *file, struct folio *folio)
{
	struct inode       *inode = folio->mapping->host;
	struct super_block *sb    = inode->i_sb;
	u64                 b_first, b_last, b;
	u32                 k_first, len_in_b_last, fub;
	u32                 folio_offset = 0;  /* current write offset in folio */
	u8                 *dst;
	int                 ret;

	/* Single-page folios are guaranteed by mapping_set_folio_order_range. */
	if (WARN_ON_ONCE(folio_size(folio) != BEAMFS_BLOCK_SIZE)) {
		ret = -EIO;
		goto out_unlock;
	}

	ret = beamfs_inline_folio_coverage(inode, folio->index,
					   &b_first, &k_first,
					   &b_last, &len_in_b_last,
					   &fub);
	if (ret == -ERANGE) {
		/* Folio at or beyond i_size: zero-fill per VFS convention. */
		dst = kmap_local_folio(folio, 0);
		memset(dst, 0, BEAMFS_BLOCK_SIZE);
		flush_dcache_folio(folio);
		kunmap_local(dst);
		folio_end_read(folio, true);
		return 0;
	}
	if (ret < 0)
		goto out_unlock;

	dst = kmap_local_folio(folio, 0);

	for (b = b_first; b <= b_last; b++) {
		u32 slice_offset_in_block;
		u32 slice_length;
		u64 phys = 0;

		if (b == b_first) {
			slice_offset_in_block = k_first;
			slice_length = (b == b_last)
				? len_in_b_last
				: (BEAMFS_DATA_INLINE_BYTES - k_first);
		} else if (b == b_last) {
			slice_offset_in_block = 0;
			slice_length = len_in_b_last;
		} else {
			/* Intermediate block (tri-block case): full INLINE block */
			slice_offset_in_block = 0;
			slice_length = BEAMFS_DATA_INLINE_BYTES;
		}

		ret = beamfs_inline_lookup_phys(inode, b, &phys);
		if (ret < 0) {
			kunmap_local(dst);
			goto out_unlock;
		}

		if (phys == 0) {
			/* HOLE: zero the slice for this block. */
			memset(dst + folio_offset, 0, slice_length);
		} else {
			ret = beamfs_inline_decode_block_into_buf(
				sb, phys, inode, b,
				dst + folio_offset,
				slice_offset_in_block, slice_length);
			if (ret < 0) {
				kunmap_local(dst);
				goto out_unlock;
			}
		}

		folio_offset += slice_length;
	}

	/* Zero the trailing portion [fub, BEAMFS_BLOCK_SIZE). */
	if (fub < BEAMFS_BLOCK_SIZE)
		memset(dst + fub, 0, BEAMFS_BLOCK_SIZE - fub);
	flush_dcache_folio(folio);
	kunmap_local(dst);

	folio_end_read(folio, true);
	return 0;

out_unlock:
	folio_unlock(folio);
	return ret;
}

/* ------------------------------------------------------------------------- */
/* readahead -- per-folio loop on top of read_folio.                         */
/* ------------------------------------------------------------------------- */
static void beamfs_inline_readahead(struct readahead_control *rac)
{
	struct folio *folio;

	while ((folio = readahead_folio(rac)))
		beamfs_inline_read_folio(rac->file, folio);
}

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
static int beamfs_inline_write_begin(const struct kiocb *iocb,
				     struct address_space *mapping,
				     loff_t pos, unsigned int len,
				     struct folio **foliop, void **fsdata)
{
	pgoff_t       index;
	struct folio *folio;
	int           ret;

	/* multi-block scope: 1 or 2 underlying INLINE disk blocks. */
	if (pos < 0 || len == 0)
		return -EINVAL;

	index = pos >> PAGE_SHIFT;

	folio = __filemap_get_folio(mapping, index,
				   FGP_WRITEBEGIN | FGP_NOFS,
				   mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	/* If folio already has the data, nothing more to do. */
	if (folio_test_uptodate(folio)) {
		*foliop = folio;
		return 0;
	}

	/* Read the existing block (if allocated) to populate the folio. */
	ret = beamfs_inline_read_folio(NULL, folio);
	if (ret < 0) {
		folio_unlock(folio);
		folio_put(folio);
		return ret;
	}

	/* read_folio unlocked the folio on success; re-lock for the write. */
	folio_lock(folio);
	*foliop = folio;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* write_end (v2 INLINE)                                                     */
/*                                                                           */
/* Standard kernel pattern: flush dcache, mark folio uptodate + dirty,       */
/* update i_size if the write extended the file, then release the folio.    */
/* The actual RS encode + disk write happens later in writepages.            */
/* ------------------------------------------------------------------------- */
static int beamfs_inline_write_end(const struct kiocb *iocb,
				   struct address_space *mapping,
				   loff_t pos, unsigned int len,
				   unsigned int copied,
				   struct folio *folio, void *fsdata)
{
	struct inode *inode = mapping->host;
	loff_t        new_i_size;

	flush_dcache_folio(folio);

	if (!folio_test_uptodate(folio))
		folio_mark_uptodate(folio);
	filemap_dirty_folio(mapping, folio);

	new_i_size = pos + copied;
	if (new_i_size > i_size_read(inode)) {
		i_size_write(inode, new_i_size);
		mark_inode_dirty(inode);
	}

	folio_unlock(folio);
	folio_put(folio);
	return copied;
}

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
static int beamfs_inline_writeback_folio(struct inode *inode,
					 struct super_block *sb,
					 struct folio *folio)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	u64           b_first, b_last, b;
	u32           k_first, len_in_b_last, fub;
	u32           folio_offset = 0;
	u8           *folio_buf = NULL;
	u8           *scratch;
	unsigned int  sb_idx;
	int           ret;

	/* RMW scratch must be heap-allocated: 3824 bytes is too large for the
	 * kernel stack on aarch64 (8K) given existing frame pressure in super.c.
	 */
	scratch = kmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
	if (!scratch)
		return -ENOMEM;

	folio_lock(folio);

	if (!folio_test_dirty(folio) || folio->mapping == NULL) {
		folio_unlock(folio);
		kfree(scratch);
		return 0;
	}

	ret = beamfs_inline_folio_coverage(inode, folio->index,
					   &b_first, &k_first,
					   &b_last, &len_in_b_last,
					   &fub);
	if (ret == -ERANGE) {
		/* Folio at or beyond i_size: nothing to writeback. */
		folio_clear_dirty_for_io(folio);
		folio_unlock(folio);
		kfree(scratch);
		return 0;
	}
	if (ret < 0) {
		folio_unlock(folio);
		kfree(scratch);
		return ret;
	}

	folio_clear_dirty_for_io(folio);
	folio_start_writeback(folio);

	folio_buf = kmap_local_folio(folio, 0);

	for (b = b_first; b <= b_last; b++) {
		u32                 slice_offset_in_block;
		u32                 slice_length;
		u64                 phys = 0;
		struct buffer_head *bh;

		if (b == b_first) {
			slice_offset_in_block = k_first;
			slice_length = (b == b_last)
				? len_in_b_last
				: (BEAMFS_DATA_INLINE_BYTES - k_first);
		} else if (b == b_last) {
			slice_offset_in_block = 0;
			slice_length = len_in_b_last;
		} else {
			/* Intermediate block (tri-block case): full INLINE block */
			slice_offset_in_block = 0;
			slice_length = BEAMFS_DATA_INLINE_BYTES;
		}

		/* Serialize block allocation against concurrent writeback. */
		mutex_lock(&fi->i_alloc_mutex);
		ret = beamfs_inline_lookup_or_alloc_phys(inode, b, &phys);
		mutex_unlock(&fi->i_alloc_mutex);
		if (ret < 0)
			goto fail_kunmap;

		bh = sb_bread(sb, phys);
		if (!bh) {
			pr_err_ratelimited("beamfs/inline: writeback_folio: sb_bread phys=%llu failed\n",
					  (unsigned long long)phys);
			ret = -EIO;
			goto fail_kunmap;
		}

		/* RMW: decode existing block contents into scratch. A freshly
		 * allocated block is zero-init'd by lookup_or_alloc_phys, which
		 * decodes as 16 zero subblocks (RS-trivial valid codeword).
		 */
		ret = beamfs_inline_decode_block_into_buf(sb, phys, inode, b,
							  scratch, 0,
							  BEAMFS_DATA_INLINE_BYTES);
		if (ret < 0) {
			brelse(bh);
			goto fail_kunmap;
		}

		/* Splice the folio's contribution into scratch at the right offset. */
		memcpy(scratch + slice_offset_in_block,
		       folio_buf + folio_offset, slice_length);

		/* Re-scatter scratch into bh: 16 segments of 239 bytes. */
		for (sb_idx = 0; sb_idx < BEAMFS_DATA_INLINE_SUBBLOCKS; sb_idx++) {
			memcpy((u8 *)bh->b_data + (size_t)sb_idx * BEAMFS_SUBBLOCK_TOTAL,
			       scratch + (size_t)sb_idx * BEAMFS_SUBBLOCK_DATA,
			       BEAMFS_SUBBLOCK_DATA);
		}

		/* RS encode all 16 subblocks. */
		ret = beamfs_rs_encode_region(
			(u8 *)bh->b_data, BEAMFS_SUBBLOCK_TOTAL,
			(u8 *)bh->b_data + BEAMFS_SUBBLOCK_DATA, BEAMFS_SUBBLOCK_TOTAL,
			BEAMFS_SUBBLOCK_DATA, BEAMFS_DATA_INLINE_SUBBLOCKS);
		if (ret < 0) {
			pr_err_ratelimited("beamfs/inline: writeback_folio: rs_encode_region failed: %d\n",
					  ret);
			brelse(bh);
			goto fail_kunmap;
		}

		/* Zero the 16-byte pad zone (4080..4096). */
		memset((u8 *)bh->b_data + BEAMFS_DATA_INLINE_TOTAL, 0,
		       BEAMFS_DATA_INLINE_PAD);

		mark_buffer_dirty(bh);
		ret = sync_dirty_buffer(bh);
		brelse(bh);
		if (ret < 0)
			goto fail_kunmap;

		folio_offset += slice_length;
	}

	kunmap_local(folio_buf);
	folio_end_writeback(folio);
	folio_unlock(folio);
	kfree(scratch);
	return 0;

fail_kunmap:
	kunmap_local(folio_buf);
	folio_end_writeback(folio);
	folio_unlock(folio);
	kfree(scratch);
	return ret;
}

static int beamfs_inline_writepages(struct address_space *mapping,
				    struct writeback_control *wbc)
{
	struct inode       *inode = mapping->host;
	struct super_block *sb    = inode->i_sb;
	struct folio_batch  fbatch;
	pgoff_t             index, end;
	int                 ret = 0;

	folio_batch_init(&fbatch);

	/* VFS-standard pgoff_t range from writeback_control. */
	index = wbc->range_start >> PAGE_SHIFT;
	if (wbc->range_end == LLONG_MAX)
		end = ULONG_MAX;
	else
		end = wbc->range_end >> PAGE_SHIFT;

	while (index <= end) {
		unsigned int nr_folios;
		unsigned int i;

		nr_folios = filemap_get_folios_tag(mapping, &index, end,
						   PAGECACHE_TAG_DIRTY,
						   &fbatch);
		if (nr_folios == 0)
			break;

		for (i = 0; i < nr_folios; i++) {
			struct folio *folio = fbatch.folios[i];

			ret = beamfs_inline_writeback_folio(inode, sb, folio);
			if (ret < 0)
				goto out_release;
		}

		folio_batch_release(&fbatch);
	}

	return 0;

out_release:
	folio_batch_release(&fbatch);
	return ret;
}

/* ------------------------------------------------------------------------- */
/* write_iter entry point                                                    */
/* ------------------------------------------------------------------------- */
static ssize_t beamfs_inline_file_write_iter(struct kiocb *iocb,
					     struct iov_iter *from)
{
	return generic_perform_write(iocb, from);
}

/* ------------------------------------------------------------------------- */
/* Public ops structures                                                     */
/* ------------------------------------------------------------------------- */

const struct address_space_operations beamfs_inline_aops = {
	.read_folio       = beamfs_inline_read_folio,
	.writepages       = beamfs_inline_writepages,
	.readahead        = beamfs_inline_readahead,
	.write_begin      = beamfs_inline_write_begin,
	.write_end        = beamfs_inline_write_end,
	.dirty_folio      = filemap_dirty_folio,
};

const struct file_operations beamfs_inline_file_operations = {
	.llseek      = generic_file_llseek,
	.read_iter   = generic_file_read_iter,
	.write_iter  = beamfs_inline_file_write_iter,
	.mmap        = generic_file_mmap,
	.fsync       = generic_file_fsync,
	.splice_read = filemap_splice_read,
};

/* ------------------------------------------------------------------------- */
/* Truncate support (sub-step 6 INLINE-MULTIBLOCK)                           */
/*                                                                           */
/* Scope: direct + single indirect. The allocator (lookup_or_alloc_phys)    */
/* does not yet allocate dindirect/tindirect in v0.1.x baseline, so          */
/* truncate has nothing to free at those levels here. Maximum file size in   */
/* the current baseline is (BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS) *   */
/* 3824 = 524 blocks = ~1.91 MiB.                                            */
/*                                                                           */
/* Multi-level indirect (dindirect ~1 GiB, tindirect ~512 GiB) extends this  */
/* ceiling in subsequent v0.1.x increments without a format bump (see        */
/* BEAMFS_DINDIRECT_PTRS / BEAMFS_TINDIRECT_PTRS in beamfs.h and the         */
/* Documentation/format-v5.md section on Indirect addressing). The 64BIT +   */
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
			beamfs_free_block(sb, blk);
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
		ptrs = (__le64 *)ibh->b_data;

		if (b_first_freed >= BEAMFS_DIRECT_BLOCKS)
			slot_first = b_first_freed - BEAMFS_DIRECT_BLOCKS;
		else
			slot_first = 0;

		for (j = slot_first; j < nptrs; j++) {
			u64 blk = le64_to_cpu(ptrs[j]);

			if (blk) {
				beamfs_free_block(sb, blk);
				ptrs[j] = 0;
			}
		}

		mark_buffer_dirty(ibh);
		brelse(ibh);

		/* If we freed the entire indirect range, drop the indirect
		 * block itself.
		 */
		if (slot_first == 0) {
			beamfs_free_block(sb, indirect_blk);
			fi->i_indirect = 0;
		}
	}

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

	ret = beamfs_inline_decode_block_into_buf(sb, phys, inode, b,
						  scratch, 0,
						  BEAMFS_DATA_INLINE_BYTES);
	if (ret < 0) {
		kfree(scratch);
		return ret;
	}

	memset(scratch + zero_offset, 0,
	       BEAMFS_DATA_INLINE_BYTES - zero_offset);

	bh = sb_bread(sb, phys);
	if (!bh) {
		pr_err_ratelimited("beamfs/inline: zero_tail: sb_bread phys=%llu failed\n",
				   (unsigned long long)phys);
		kfree(scratch);
		return -EIO;
	}

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
		brelse(bh);
		kfree(scratch);
		return ret;
	}

	memset((u8 *)bh->b_data + BEAMFS_DATA_INLINE_TOTAL, 0,
	       BEAMFS_DATA_INLINE_PAD);

	mark_buffer_dirty(bh);
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
