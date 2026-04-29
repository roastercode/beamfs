// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAMFS — Inode operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include "beamfs.h"

/*
 * beamfs_iget — read inode from disk into VFS
 * @sb:  superblock
 * @ino: inode number (1-based)
 *
 * Inode table starts at s_inode_table_blk.
 * Each block holds BEAMFS_BLOCK_SIZE / sizeof(beamfs_inode) inodes.
 */
struct inode *beamfs_iget(struct super_block *sb, unsigned long ino)
{
	struct beamfs_sb_info    *sbi = BEAMFS_SB(sb);
	struct beamfs_inode_info *fi;
	struct beamfs_inode      *raw;
	struct buffer_head      *bh;
	struct inode            *inode;
	unsigned long            inodes_per_block;
	unsigned long            block, offset;
	__u32                    crc;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	/* Already in cache */
	if (!beamfs_inode_is_new(inode))
		return inode;

	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	block  = le64_to_cpu(sbi->s_beamfs_sb->s_inode_table_blk)
		 + (ino - 1) / inodes_per_block;
	offset = (ino - 1) % inodes_per_block;

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("beamfs: unable to read inode block %lu\n", block);
		iget_failed(inode);
		return ERR_PTR(-EIO);
	}

	raw = (struct beamfs_inode *)bh->b_data + offset;

	/*
	 * Integrity check, two stages.
	 *
	 * Stage A: CRC32. The nominal path. >99% of inode reads are
	 * expected to match here at low cost; CRC32 is hardware-accelerated
	 * by crc32_le on architectures that support it.
	 *
	 * Stage B: RS FEC, only invoked if CRC32 fails AND the format
	 * declares RS protection on inodes (s_data_protection_scheme ==
	 * INODE_UNIVERSAL). For images formatted under the legacy
	 * INODE_OPT_IN scheme (v0.1.0 / v0.2.0 baseline), no per-inode
	 * parity was ever written by mkfs, so attempting RS would be
	 * useless or actively harmful (random bytes interpreted as parity
	 * could push the decoder into a false correction).
	 *
	 * After a successful RS correction we re-verify CRC32 on the
	 * corrected buffer before accepting the inode. This guards
	 * against the rare case where an SEU on the parity field itself
	 * makes decode_rs8 return success while the data has actually
	 * been altered toward a wrong codeword.
	 */
	crc = beamfs_crc32(raw, offsetof(struct beamfs_inode, i_crc32));
	if (crc != le32_to_cpu(raw->i_crc32)) {
		u32 scheme = le32_to_cpu(
			BEAMFS_SB(sb)->s_beamfs_sb->s_data_protection_scheme);
		int nerr;

		if (scheme != BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL) {
			pr_err("beamfs: inode %lu CRC32 mismatch (no RS available, scheme=%u)\n",
			       ino, scheme);
			brelse(bh);
			iget_failed(inode);
			return ERR_PTR(-EIO);
		}

		{
			int positions[BEAMFS_RS_PARITY / 2];

			nerr = beamfs_rs_decode((u8 *)raw, BEAMFS_INODE_RS_DATA,
					       raw->i_reserved,
					       positions,
					       BEAMFS_RS_PARITY / 2);
			if (nerr < 0) {
				pr_err("beamfs: inode %lu uncorrectable\n", ino);
				brelse(bh);
				iget_failed(inode);
				return ERR_PTR(-EIO);
			}

			crc = beamfs_crc32(raw, offsetof(struct beamfs_inode, i_crc32));
			if (crc != le32_to_cpu(raw->i_crc32)) {
				pr_err("beamfs: inode %lu CRC32 mismatch after RS correction\n",
				       ino);
				brelse(bh);
				iget_failed(inode);
				return ERR_PTR(-EIO);
			}

			/* Log the RS event with position list for entropy. nerr is
			 * the total symbol count (data + parity); positions[] holds
			 * only DATA positions used by the entropy estimator. */
			if (nerr > 0) {
				unsigned int np = (unsigned int)nerr;

				if (np > BEAMFS_RS_PARITY / 2)
					np = BEAMFS_RS_PARITY / 2;
				beamfs_log_rs_event(sb, (u64)ino,
						   positions, np,
						   BEAMFS_INODE_RS_DATA);
			}
			mark_buffer_dirty(bh);
			pr_warn("beamfs: inode %lu corrected by RS FEC (%d symbols)\n",
				ino, nerr);
		}
	}

	fi = BEAMFS_I(inode);

	/* Populate VFS inode */
	inode->i_mode  = le16_to_cpu(raw->i_mode);
	inode->i_uid   = make_kuid(sb->s_user_ns, le32_to_cpu(raw->i_uid));
	inode->i_gid   = make_kgid(sb->s_user_ns, le32_to_cpu(raw->i_gid));
	set_nlink(inode, le16_to_cpu(raw->i_nlink));
	inode->i_size  = le64_to_cpu(raw->i_size);

	inode_set_atime(inode,
			le64_to_cpu(raw->i_atime) / NSEC_PER_SEC,
			le64_to_cpu(raw->i_atime) % NSEC_PER_SEC);
	inode_set_mtime(inode,
			le64_to_cpu(raw->i_mtime) / NSEC_PER_SEC,
			le64_to_cpu(raw->i_mtime) % NSEC_PER_SEC);
	inode_set_ctime(inode,
			le64_to_cpu(raw->i_ctime) / NSEC_PER_SEC,
			le64_to_cpu(raw->i_ctime) % NSEC_PER_SEC);

	/* Copy block pointers to in-memory inode */
	memcpy(fi->i_direct, raw->i_direct, sizeof(fi->i_direct));
	fi->i_indirect  = raw->i_indirect;
	fi->i_dindirect = raw->i_dindirect;
	fi->i_tindirect = raw->i_tindirect;
	fi->i_flags     = le32_to_cpu(raw->i_flags);

	/* Set ops based on file type */
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op  = &beamfs_dir_inode_operations;
		inode->i_fop = &beamfs_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &beamfs_file_inode_operations;
		if (sbi->s_scheme == BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE) {
			/*
			 * v2 INLINE: per-block RS FEC requires gather/scatter
			 * across 16 subblocks per disk block. Single-page
			 * folios only; large folios deferred to v2.1.
			 */
			inode->i_fop = &beamfs_inline_file_operations;
			inode->i_mapping->a_ops = &beamfs_inline_aops;
			mapping_set_folio_order_range(inode->i_mapping, 0, 0);
		} else {
			/* legacy iomap path (scheme=5 INODE_UNIVERSAL) */
			inode->i_fop = &beamfs_file_operations;
			inode->i_mapping->a_ops = &beamfs_aops;
		}
	} else {
		/* Special files: use generic */
		init_special_inode(inode, inode->i_mode, 0);
	}

	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}
