// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAMFS — File operations for BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE (v2)
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
		*phys_out = le64_to_cpu(fi->i_direct[iblock_logical]);
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
/* read_folio (v2 INLINE) -- per-block RS(255,239) FEC on data blocks.       */
/*                                                                           */
/* Each disk block is laid out as 16 interleaved subblocks of 255 bytes      */
/* (239 user data || 16 parity), followed by 16 bytes of zero pad. We:       */
/*   1) look up the physical block for folio->index                          */
/*   2) handle HOLE (zero-fill, return 0)                                    */
/*   3) sb_bread the disk block                                              */
/*   4) RS-decode all 16 subblocks in place (corrects up to 8 byte symbols   */
/*      per subblock, i.e. 128 bytes per disk block)                         */
/*   5) on uncorrectable: -EIO, no folio uptodate marking                    */
/*   6) on success: gather 16 * 239 bytes into the folio, zero the pad zone  */
/*   7) if any subblock was corrected: write back the repaired disk block    */
/*      synchronously (durable autonomic repair, mirrors alloc.c bitmap path)*/
/*                                                                           */
/* Single-page folios only (mapping_set_folio_order_range(0,0) in inode      */
/* setup). MIL-STD-883 SEE coverage: validates BEAMFS resistance to RadFI    */
/* single-bit and multi-byte payload corruption injected at submit_bio.      */
/* ------------------------------------------------------------------------- */
static int beamfs_inline_read_folio(struct file *file, struct folio *folio)
{
	struct inode             *inode = folio->mapping->host;
	struct super_block       *sb    = inode->i_sb;
	struct buffer_head       *bh    = NULL;
	u64                       iblock_logical;
	u64                       phys = 0;
	int                       rs_results[BEAMFS_DATA_INLINE_SUBBLOCKS];
	int                       rs_positions[BEAMFS_DATA_INLINE_SUBBLOCKS *
					   (BEAMFS_RS_PARITY / 2)];
	bool                      corrected = false;
	bool                      uncorrectable = false;
	u8                       *dst;
	unsigned int              i;
	int                       ret;

	/* Single-page folios are guaranteed by mapping_set_folio_order_range. */
	if (WARN_ON_ONCE(folio_size(folio) != BEAMFS_BLOCK_SIZE)) {
		ret = -EIO;
		goto out_unlock;
	}

	iblock_logical = folio->index;

	ret = beamfs_inline_lookup_phys(inode, iblock_logical, &phys);
	if (ret < 0)
		goto out_unlock;

	if (phys == 0) {
		/* HOLE: zero the folio, mark uptodate, done. */
		dst = kmap_local_folio(folio, 0);
		memset(dst, 0, BEAMFS_BLOCK_SIZE);
		flush_dcache_folio(folio);
		kunmap_local(dst);
		folio_end_read(folio, true);
		return 0;
	}

	bh = sb_bread(sb, phys);
	if (!bh) {
		pr_err_ratelimited("beamfs/inline: sb_bread failed phys=%llu\n",
				   (unsigned long long)phys);
		ret = -EIO;
		goto out_unlock;
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
					   (unsigned long long)iblock_logical,
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
					    (unsigned long long)iblock_logical,
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
	 * Gather: 16 segments of 239 user bytes from the buffer head into
	 * the folio. Skip 16 parity bytes between subblocks. Zero the pad
	 * zone (3824..4096) in the folio.
	 */
	dst = kmap_local_folio(folio, 0);
	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		memcpy(dst + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       (u8 *)bh->b_data + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       BEAMFS_SUBBLOCK_DATA);
	}
	memset(dst + BEAMFS_DATA_INLINE_BYTES, 0, BEAMFS_DATA_INLINE_PAD +
	       (BEAMFS_BLOCK_SIZE - BEAMFS_DATA_INLINE_TOTAL -
		BEAMFS_DATA_INLINE_PAD));
	flush_dcache_folio(folio);
	kunmap_local(dst);

	/*
	 * Durable autonomic repair: if RS corrected any subblock, write the
	 * repaired disk block back synchronously so the on-disk image is
	 * healed before the next read. Same pattern as the bitmap recovery
	 * path in alloc.c.
	 */
	if (corrected) {
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	}

	brelse(bh);
	folio_end_read(folio, true);
	return 0;

out_brelse:
	brelse(bh);
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
/* write_begin (v2 INLINE, single-block scope)                               */
/*                                                                           */
/* Provides a folio for the write to land into. The actual encoding to disk  */
/* (RS encode + sync) happens in writepages.                                 */
/*                                                                           */
/* SCOPE LIMITATION (v2.0): only single-block files (file_offset + len <=    */
/* BEAMFS_DATA_INLINE_BYTES = 3824). Multi-block INLINE writes are rejected  */
/* with -EFBIG. The reason is the impedance mismatch between the VFS folio  */
/* model (4096 user bytes per page) and the INLINE layout (3824 user bytes  */
/* per disk block). Multi-block INLINE write is deferred to the v2.x roadmap*/
/* per Documentation/format-v4.md section 7.5.                               */
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
	struct inode *inode = mapping->host;
	struct folio *folio;
	int           ret;

	/* v2.0 scope: single-block only. */
	if (pos < 0 || len == 0)
		return -EINVAL;
	if ((u64)pos + len > BEAMFS_DATA_INLINE_BYTES) {
		pr_warn_ratelimited("beamfs/inline: write_begin: pos+len=%llu exceeds single-block scope (3824 bytes); multi-block INLINE write is v2.x roadmap\n",
				   (unsigned long long)((u64)pos + len));
		return -EFBIG;
	}

	folio = __filemap_get_folio(mapping, 0,
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
/* writepages (v2 INLINE)                                                    */
/*                                                                           */
/* For each dirty folio in the mapping:                                      */
/*   1) Look up or allocate the physical block backing the folio.            */
/*   2) sb_bread the physical block (or use a freshly zero-init'd buffer).   */
/*   3) Scatter 3824 user bytes from the folio into the buffer head, one     */
/*      239-byte run per subblock, leaving the 16 parity bytes untouched.    */
/*   4) RS encode all 16 subblocks via beamfs_rs_encode_region().            */
/*   5) Zero the 16-byte pad zone (offset 4080..4096).                       */
/*   6) mark_buffer_dirty + sync_dirty_buffer for autonomic durability.      */
/*   7) folio_clear_dirty_for_io + folio_end_writeback.                      */
/*                                                                           */
/* Single-block scope (v2.0): only folio->index == 0 is processed; any       */
/* other dirty folio is silently skipped (write_begin already enforces the   */
/* single-block constraint at write entry).                                  */
/* ------------------------------------------------------------------------- */
static int beamfs_inline_writepages(struct address_space *mapping,
				    struct writeback_control *wbc)
{
	struct inode       *inode = mapping->host;
	struct super_block *sb    = inode->i_sb;
	struct folio_batch  fbatch;
	struct folio       *folio;
	unsigned int        i;
	int                 ret = 0;

	folio_batch_init(&fbatch);

	while (filemap_get_folios_tag(mapping, &wbc->range_start,
				      wbc->range_end >> PAGE_SHIFT,
				      PAGECACHE_TAG_DIRTY, &fbatch)) {
		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct buffer_head *bh;
			u64                 phys = 0;
			u8                 *src;
			unsigned int        sb_idx;

			folio = fbatch.folios[i];

			/* v2.0 scope: only process folio index 0. */
			if (folio->index != 0) {
				pr_warn_ratelimited("beamfs/inline: writepages: skipping folio index %lu (single-block scope)\n",
						   folio->index);
				continue;
			}

			folio_lock(folio);

			if (!folio_test_dirty(folio) ||
			    folio->mapping != mapping) {
				folio_unlock(folio);
				continue;
			}

			ret = beamfs_inline_lookup_or_alloc_phys(inode, 0, &phys);
			if (ret < 0) {
				folio_unlock(folio);
				goto out_release;
			}

			bh = sb_bread(sb, phys);
			if (!bh) {
				pr_err_ratelimited("beamfs/inline: writepages: sb_bread phys=%llu failed\n",
						  (unsigned long long)phys);
				ret = -EIO;
				folio_unlock(folio);
				goto out_release;
			}

			folio_clear_dirty_for_io(folio);
			folio_start_writeback(folio);

			/* Scatter folio bytes into bh: 16 segments of 239 bytes. */
			src = kmap_local_folio(folio, 0);
			for (sb_idx = 0; sb_idx < BEAMFS_DATA_INLINE_SUBBLOCKS; sb_idx++) {
				memcpy((u8 *)bh->b_data + (size_t)sb_idx * BEAMFS_SUBBLOCK_TOTAL,
				       src + (size_t)sb_idx * BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_SUBBLOCK_DATA);
			}
			kunmap_local(src);

			/* RS encode all 16 subblocks: parity at offset 239 within each 255-byte stride. */
			ret = beamfs_rs_encode_region(
				(u8 *)bh->b_data, BEAMFS_SUBBLOCK_TOTAL,
				(u8 *)bh->b_data + BEAMFS_SUBBLOCK_DATA, BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA, BEAMFS_DATA_INLINE_SUBBLOCKS);
			if (ret < 0) {
				pr_err_ratelimited("beamfs/inline: writepages: rs_encode_region failed: %d\n",
						  ret);
				brelse(bh);
				folio_end_writeback(folio);
				folio_unlock(folio);
				goto out_release;
			}

			/* Zero the 16-byte pad zone (4080..4096). */
			memset((u8 *)bh->b_data + BEAMFS_DATA_INLINE_TOTAL, 0,
			       BEAMFS_DATA_INLINE_PAD);

			/* Autonomic durability: sync the encoded block to disk. */
			mark_buffer_dirty(bh);
			ret = sync_dirty_buffer(bh);
			brelse(bh);

			folio_end_writeback(folio);
			folio_unlock(folio);

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
	.fsync       = generic_file_fsync,
	.splice_read = filemap_splice_read,
};
