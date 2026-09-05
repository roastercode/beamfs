// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Inode operations
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include "beamfs.h"

/*
 * beamfs_iget - read inode from disk into VFS
 * @sb:  superblock
 * @ino: inode number (1-based)
 *
 * Inode table starts at s_inode_table_blk.
 * Each block holds BEAMFS_BLOCK_SIZE / sizeof(beamfs_inode) inodes.
 */
/*
 * Split a nanosecond count into the seconds and nanoseconds a timespec64
 * wants, with the nanosecond part never negative.
 */
static void beamfs_split_ns(s64 ns, s64 *sec, long *nsec)
{
	s32 rem;

	*sec = div_s64_rem(ns, NSEC_PER_SEC, &rem);
	if (rem < 0) {
		*sec -= 1;
		rem += NSEC_PER_SEC;
	}
	*nsec = rem;
}

#define beamfs_set_time_from_ns(inode, setter, ns)			\
	do {								\
		s64  _sec;						\
		long _nsec;						\
									\
		beamfs_split_ns((ns), &_sec, &_nsec);			\
		setter((inode), _sec, _nsec);				\
	} while (0)

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

	bh = beamfs_bread(sb, block, "inode table");
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
	 * declares RS protection on inodes. Two activation paths:
	 *   - s_data_protection_scheme == INODE_UNIVERSAL (legacy v4)
	 *   - s_feat_incompat & BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS (v5)
	 * Both paths share the same on-disk parity layout in i_reserved
	 * [0..15] (16 bytes covering BEAMFS_INODE_RS_DATA = 172 data
	 * bytes). The kernel write path always emits this parity
	 * (namei.c::beamfs_write_inode_raw) and mkfs.beamfs always seeds
	 * it at format time (rs_encode_inode), so the decoder is safe to
	 * invoke whenever either path is active.
	 *
	 * After a successful RS correction we re-verify CRC32 on the
	 * corrected buffer before accepting the inode. This guards
	 * against the rare case where an SEU on the parity field itself
	 * makes decode_rs8 return success while the data has actually
	 * been altered toward a wrong codeword.
	 */
	/*
	 * A free inode is not a corrupt one.
	 *
	 * Free slots are zeroed, i_crc32 included, and crc32 of a zeroed
	 * buffer is 0xf288b395 rather than zero -- so every read of a free
	 * inode fails the check below by construction. Dozens of them
	 * appeared in twenty milliseconds during one test run, reported as
	 * "CRC32 mismatch", which is both the wrong error and the wrong
	 * diagnosis: nothing was corrupt, something had followed a stale
	 * reference.
	 *
	 * ESTALE is what the VFS expects here, and it is what ext2 returns
	 * for an inode with no links.
	 */
	if (le16_to_cpu(raw->i_mode) == 0) {
		brelse(bh);
		iget_failed(inode);
		return ERR_PTR(-ESTALE);
	}

	crc = beamfs_crc32(raw, offsetof(struct beamfs_inode, i_crc32));
	if (crc != le32_to_cpu(raw->i_crc32)) {
		u32 scheme = le32_to_cpu(
			BEAMFS_SB(sb)->s_beamfs_sb->s_data_protection_scheme);
		int nerr;

		if (scheme != BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL &&
		    !(BEAMFS_SB(sb)->s_feat_incompat &
		      BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS)) {
			pr_err("beamfs: inode %lu CRC32 mismatch (no RS available, scheme=%u feat_incompat=0x%016llx)\n",
			       ino, scheme,
			       (unsigned long long)BEAMFS_SB(sb)->s_feat_incompat);
			brelse(bh);
			iget_failed(inode);
			return ERR_PTR(-EIO);
		}

		{
			int positions[BEAMFS_RS_PARITY / 2];

			nerr = beamfs_rs_decode((u8 *)raw, BEAMFS_INODE_RS_DATA,
					       raw->i_reserved,
					       positions,
					       BEAMFS_RS_PARITY / 2,
				"inode");
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
			 * only DATA positions used by the entropy estimator.
			 */
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

	/*
	 * Times before 1970 are negative, and there is nothing exotic
	 * about them: tar preserves them, build systems set them, and a
	 * file dated 1960 is a file. le64_to_cpu returns unsigned, so
	 * dividing it turned -315M seconds into 1.8e19 -- generic/258
	 * reports it as "Timestamp wrapped".
	 *
	 * div_s64_rem rather than / and %, because C truncates toward
	 * zero: -1.5e9 ns would give -1 second and -5e8 nanoseconds, and
	 * a negative nanosecond field is not a time the VFS can use. The
	 * floor form gives -2 seconds and +5e8 nanoseconds, which is the
	 * same instant expressed the way timespec64 requires.
	 */
	beamfs_set_time_from_ns(inode, inode_set_atime,
				(s64)le64_to_cpu(raw->i_atime));
	beamfs_set_time_from_ns(inode, inode_set_mtime,
				(s64)le64_to_cpu(raw->i_mtime));
	beamfs_set_time_from_ns(inode, inode_set_ctime,
				(s64)le64_to_cpu(raw->i_ctime));

	/* Copy block pointers to in-memory inode */
	memcpy(fi->i_direct, raw->i_direct, sizeof(fi->i_direct));
	fi->i_indirect  = raw->i_indirect;
	fi->i_dindirect = raw->i_dindirect;
	fi->i_tindirect = raw->i_tindirect;
	fi->i_flags     = le32_to_cpu(raw->i_flags);

	/* Layer 1 defense: mark reserved inodes (canary) as immutable.
	 * VFS rejects open(O_TRUNC), setattr(ATTR_SIZE), unlink, chmod
	 * with EPERM before reaching beamfs hooks. Layer 2 in alloc.c
	 * provides the safety net if VFS check is bypassed (corruption).
	 */
	if (beamfs_ino_is_reserved((u64)ino) && ino != BEAMFS_RESERVED_INO_ROOT)
		inode->i_flags |= S_IMMUTABLE;

	/* Set ops based on file type */
	if (S_ISDIR(inode->i_mode)) {
		inode->i_op  = &beamfs_dir_inode_operations;
		inode->i_fop = &beamfs_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		if (sbi->s_scheme == BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE) {
			/*
			 * v2 INLINE: per-block RS FEC requires gather/scatter
			 * across 16 subblocks per disk block. Single-page
			 * folios only; large folios deferred to v2.1.
			 * Dedicated i_op carries setattr/truncate (sub-step 6).
			 */
			inode->i_op = &beamfs_inline_inode_operations;
			inode->i_fop = &beamfs_inline_file_operations;
			inode->i_mapping->a_ops = &beamfs_inline_aops;
			mapping_set_folio_order_range(inode->i_mapping, 0, 0);
		} else {
			/* legacy iomap path (scheme=5 INODE_UNIVERSAL) */
			inode->i_op = &beamfs_file_inode_operations;
			inode->i_fop = &beamfs_file_operations;
			inode->i_mapping->a_ops = &beamfs_aops;
		}
	} else if (S_ISLNK(inode->i_mode)) {
		/* Fast symlink: target stored inline in i_direct[] (<= 96 b). */
		inode->i_op = &beamfs_symlink_inode_operations;
	} else {
		/* Special files: use generic */
		init_special_inode(inode, inode->i_mode, 0);
	}

	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}
