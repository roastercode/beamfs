// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - Filename / directory entry operations
 * Author: Aurélien DESBRIERES <aurelien@hackers.camp>
 *
 * Implements: create, mkdir, unlink, rmdir, link, rename
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include "beamfs.h"

/* ------------------------------------------------------------------ */
/* Helper: write a raw beamfs_inode to disk                             */
/* ------------------------------------------------------------------ */

int beamfs_write_inode_raw(struct inode *inode)
{
	struct super_block      *sb  = inode->i_sb;
	struct beamfs_sb_info    *sbi = BEAMFS_SB(sb);
	struct beamfs_inode_info *fi  = BEAMFS_I(inode);
	struct beamfs_inode      *raw;
	struct buffer_head      *bh;
	unsigned long            inodes_per_block;
	unsigned long            block, offset;

	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	block  = le64_to_cpu(sbi->s_beamfs_sb->s_inode_table_blk)
		 + (inode->i_ino - 1) / inodes_per_block;
	offset = (inode->i_ino - 1) % inodes_per_block;

	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	raw = (struct beamfs_inode *)bh->b_data + offset;

	raw->i_mode   = cpu_to_le16(inode->i_mode);
	raw->i_uid    = cpu_to_le32(i_uid_read(inode));
	raw->i_gid    = cpu_to_le32(i_gid_read(inode));
	raw->i_nlink  = cpu_to_le16(inode->i_nlink);
	raw->i_size   = cpu_to_le64(inode->i_size);
	raw->i_atime  = cpu_to_le64(inode_get_atime_sec(inode) * NSEC_PER_SEC
				     + inode_get_atime_nsec(inode));
	raw->i_mtime  = cpu_to_le64(inode_get_mtime_sec(inode) * NSEC_PER_SEC
				     + inode_get_mtime_nsec(inode));
	raw->i_ctime  = cpu_to_le64(inode_get_ctime_sec(inode) * NSEC_PER_SEC
				     + inode_get_ctime_nsec(inode));
	raw->i_flags  = cpu_to_le32(fi->i_flags);

	memcpy(raw->i_direct, fi->i_direct, sizeof(fi->i_direct));
	raw->i_indirect  = fi->i_indirect;
	raw->i_dindirect = fi->i_dindirect;
	raw->i_tindirect = fi->i_tindirect;

	raw->i_crc32 = beamfs_crc32(raw,
				    offsetof(struct beamfs_inode, i_crc32));

	/*
	 * Compute RS parity over the first BEAMFS_INODE_RS_DATA bytes
	 * (everything up to i_reserved, including i_crc32). Parity goes
	 * into i_reserved[0..15]; i_reserved[16..83] is forced to zero so
	 * the layout is deterministic and any non-zero byte there at read
	 * time signals a tampered inode.
	 *
	 * Under BEAMFS_DATA_PROTECTION_INODE_UNIVERSAL this parity is the
	 * authoritative correction record for the inode. mkfs.beamfs writes
	 * the equivalent parity on the root inode at format time; the
	 * kernel maintains it on every subsequent inode write.
	 */
	memset(&raw->i_reserved[BEAMFS_RS_PARITY], 0,
	       sizeof(raw->i_reserved) - BEAMFS_RS_PARITY);
	beamfs_rs_encode((u8 *)raw, BEAMFS_INODE_RS_DATA, raw->i_reserved);

	mark_buffer_dirty(bh);
	brelse(bh);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: add a directory entry to a directory inode                  */
/* ------------------------------------------------------------------ */

static int beamfs_add_dirent(struct inode *dir, const struct qstr *name,
			    u64 ino, unsigned int file_type)
{
	struct super_block      *sb = dir->i_sb;
	struct beamfs_inode_info *fi = BEAMFS_I(dir);
	struct beamfs_dir_entry  *de;
	struct buffer_head      *bh;
	unsigned int             offset;
	u64                      block_no;
	int                      i;

	/* Look for space in existing direct blocks */
	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
		block_no = le64_to_cpu(fi->i_direct[i]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			return -EIO;

		offset = 0;
		while (offset + sizeof(*de) <= BEAMFS_BLOCK_SIZE) {
			de = (struct beamfs_dir_entry *)(bh->b_data + offset);

			/*
			 * Free slot: d_ino == 0. Includes never-used trailing
			 * slots and slots freed by beamfs_del_dirent. We must
			 * scan the whole block to find a free slot, even past
			 * deleted entries; otherwise blocks fill up wastefully
			 * and ENOSPC strikes early.
			 */
			if (!de->d_ino) {
				de->d_ino       = cpu_to_le64(ino);
				de->d_name_len  = name->len;
				de->d_file_type = file_type;
				de->d_rec_len   = cpu_to_le16(
					sizeof(struct beamfs_dir_entry));
				memcpy(de->d_name, name->name, name->len);
				de->d_name[name->len] = '\0';
				mark_buffer_dirty(bh);
				brelse(bh);
				inode_set_mtime_to_ts(dir,
					current_time(dir));
				mark_inode_dirty(dir);
				return 0;
			}
			offset += sizeof(struct beamfs_dir_entry);
		}
		brelse(bh);
	}

	/* Need a new block */
	if (i >= BEAMFS_DIRECT_BLOCKS)
		return -ENOSPC;

	block_no = beamfs_alloc_block(sb);
	if (!block_no)
		return -ENOSPC;

	bh = sb_bread(sb, block_no);
	if (!bh) {
		beamfs_free_block(sb, block_no);
		return -EIO;
	}

	memset(bh->b_data, 0, BEAMFS_BLOCK_SIZE);

	de = (struct beamfs_dir_entry *)bh->b_data;
	de->d_ino       = cpu_to_le64(ino);
	de->d_name_len  = name->len;
	de->d_file_type = file_type;
	de->d_rec_len   = cpu_to_le16(sizeof(struct beamfs_dir_entry));
	memcpy(de->d_name, name->name, name->len);
	de->d_name[name->len] = '\0';

	mark_buffer_dirty(bh);
	brelse(bh);

	fi->i_direct[i] = cpu_to_le64(block_no);
	dir->i_size += BEAMFS_BLOCK_SIZE;
	inode_set_mtime_to_ts(dir, current_time(dir));
	mark_inode_dirty(dir);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: remove a directory entry from a directory                   */
/* ------------------------------------------------------------------ */

static int beamfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct super_block      *sb = dir->i_sb;
	struct beamfs_inode_info *fi = BEAMFS_I(dir);
	struct beamfs_dir_entry  *de;
	struct buffer_head      *bh;
	unsigned int             offset;
	u64                      block_no;
	int                      i;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
		block_no = le64_to_cpu(fi->i_direct[i]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			return -EIO;

		offset = 0;
		while (offset + sizeof(*de) <= BEAMFS_BLOCK_SIZE) {
			de = (struct beamfs_dir_entry *)(bh->b_data + offset);

			/*
			 * Match target by d_ino != 0 + name compare. Must scan
			 * the whole block: a previous unlink may have left a
			 * hole (d_ino == 0) before our target.
			 */
			if (de->d_ino &&
			    de->d_name_len == name->len &&
			    !memcmp(de->d_name, name->name, name->len)) {
				/* Zero out the entry (mark as free) */
				memset(de, 0, sizeof(*de));
				mark_buffer_dirty(bh);
				brelse(bh);
				inode_set_mtime_to_ts(dir,
					current_time(dir));
				mark_inode_dirty(dir);
				return 0;
			}

			offset += sizeof(struct beamfs_dir_entry);
		}
		brelse(bh);
	}

	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Helper: allocate and initialize a new VFS inode                     */
/* ------------------------------------------------------------------ */

struct inode *beamfs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block   *sb = dir->i_sb;
	struct inode         *inode;
	struct beamfs_inode_info *fi;
	u64                   ino;

	ino = beamfs_alloc_inode_num(sb);
	if (!ino)
		return ERR_PTR(-ENOSPC);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_ino    = ino;
	inode->i_size   = 0;
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));

	fi = BEAMFS_I(inode);
	memset(fi->i_direct, 0, sizeof(fi->i_direct));
	fi->i_indirect  = 0;
	fi->i_dindirect = 0;
	fi->i_flags     = 0;

	if (S_ISDIR(mode)) {
		inode->i_op  = &beamfs_dir_inode_operations;
		inode->i_fop = &beamfs_dir_operations;
		set_nlink(inode, 2);
	} else {
		struct beamfs_sb_info *sbi = BEAMFS_SB(inode->i_sb);

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
		set_nlink(inode, 1);
	}

	if (insert_inode_locked(inode) < 0) {
		make_bad_inode(inode);
		iput(inode);
		return ERR_PTR(-EIO);
	}
	mark_inode_dirty(inode);
	return inode;
}

/* ------------------------------------------------------------------ */
/* create - create a regular file                                       */
/* ------------------------------------------------------------------ */

static int beamfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;
	int           ret;

	inode = beamfs_new_inode(dir, mode | S_IFREG);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ret = beamfs_write_inode_raw(inode);
	if (ret)
		goto out_iput;

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino, 1 /* DT_REG */);
	if (ret)
		goto out_iput;

	ret = beamfs_write_inode_raw(dir);
	if (ret)
		goto out_iput;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return 0;

out_iput:
	unlock_new_inode(inode);
	iput(inode);
	return ret;
}

/* ------------------------------------------------------------------ */
/* mkdir - create a directory                                          */
/* ------------------------------------------------------------------ */

static struct dentry *beamfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int           ret;

	inode_inc_link_count(dir);

	inode = beamfs_new_inode(dir, mode | S_IFDIR);
	if (IS_ERR(inode)) {
		inode_dec_link_count(dir);
		return ERR_CAST(inode);
	}

	/* Add . and .. entries */
	ret = beamfs_add_dirent(inode, &(struct qstr)QSTR_INIT(".", 1),
			       inode->i_ino, 4 /* DT_DIR */);
	if (ret)
		goto out_fail;

	ret = beamfs_add_dirent(inode, &(struct qstr)QSTR_INIT("..", 2),
			       dir->i_ino, 4 /* DT_DIR */);
	if (ret)
		goto out_fail;

	ret = beamfs_write_inode_raw(inode);
	if (ret)
		goto out_fail;

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino,
			       4 /* DT_DIR */);
	if (ret)
		goto out_fail;

	ret = beamfs_write_inode_raw(dir);
	if (ret)
		goto out_fail;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return NULL;

out_fail:
	unlock_new_inode(inode);
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	iput(inode);
	inode_dec_link_count(dir);
	return ERR_PTR(ret);
}

/* ------------------------------------------------------------------ */
/* unlink - remove a file                                              */
/* ------------------------------------------------------------------ */

static int beamfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int           ret;

	ret = beamfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		return ret;

	inode_set_ctime_to_ts(inode, current_time(inode));
	inode_dec_link_count(inode);
	beamfs_write_inode_raw(dir);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rmdir - remove an empty directory                                   */
/* ------------------------------------------------------------------ */

static int beamfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode            *inode = d_inode(dentry);
	struct beamfs_inode_info *fi    = BEAMFS_I(inode);
	struct super_block      *sb    = inode->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry  *de;
	unsigned long            block_no;
	unsigned int             offset;
	int                      i, ret;

	/*
	 * Verify the directory is empty: scan all direct blocks and check
	 * that no entries other than '.' and '..' exist. Testing i_nlink > 2
	 * is insufficient - regular files do not increment nlink on the parent,
	 * so a directory with only files can have nlink == 2 but still be
	 * non-empty.
	 */
	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
		block_no = le64_to_cpu(fi->i_direct[i]);
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			return -EIO;

		offset = 0;
		while (offset + sizeof(*de) <= BEAMFS_BLOCK_SIZE) {
			de = (struct beamfs_dir_entry *)(bh->b_data + offset);

			/*
			 * Skip free slots (d_ino == 0). A directory with a
			 * hole followed by live entries must NOT be reported
			 * empty: keep scanning past holes.
			 */
			if (de->d_ino &&
			    !(de->d_name_len == 1 && de->d_name[0] == '.') &&
			    !(de->d_name_len == 2 && de->d_name[0] == '.'
			      && de->d_name[1] == '.')) {
				brelse(bh);
				return -ENOTEMPTY;
			}
			offset += sizeof(struct beamfs_dir_entry);
		}
		brelse(bh);
	}

	ret = beamfs_del_dirent(dir, &dentry->d_name);
	if (ret)
		return ret;

	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	inode_dec_link_count(dir);
	beamfs_write_inode_raw(dir);
	return 0;
}

/* ------------------------------------------------------------------ */
/* link - create a hard link                                           */
/* ------------------------------------------------------------------ */

static int beamfs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int           ret;

	inode_set_ctime_to_ts(inode, current_time(inode));
	inode_inc_link_count(inode);

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino, 1);
	if (ret) {
		inode_dec_link_count(inode);
		return ret;
	}

	beamfs_write_inode_raw(inode);
	beamfs_write_inode_raw(dir);
	d_instantiate(dentry, inode);
	ihold(inode);
	return 0;
}

/* ------------------------------------------------------------------ */
/* write_inode - VFS super_op: persist inode to disk                  */
/* ------------------------------------------------------------------ */

int beamfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return beamfs_write_inode_raw(inode);
}

/* ------------------------------------------------------------------ */
/* dir inode_operations - exported                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* rename - move/rename a directory entry                              */
/* ------------------------------------------------------------------ */

static int beamfs_rename(struct mnt_idmap *idmap,
			struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	int	      is_dir    = S_ISDIR(old_inode->i_mode);
	int	      ret;

	/* beamfs v1: no RENAME_EXCHANGE or RENAME_WHITEOUT */
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	if (flags & RENAME_NOREPLACE && new_inode)
		return -EEXIST;

	/*
	 * If destination exists, unlink it first.
	 * For directories: target must be empty (nlink == 2: . and ..)
	 */
	if (new_inode) {
		if (is_dir) {
			if (new_inode->i_nlink > 2)
				return -ENOTEMPTY;
		}

		ret = beamfs_del_dirent(new_dir, &new_dentry->d_name);
		if (ret)
			return ret;

		if (is_dir) {
			inode_dec_link_count(new_inode);
			inode_dec_link_count(new_inode);
			inode_dec_link_count(new_dir);
		} else {
			inode_dec_link_count(new_inode);
		}

		inode_set_ctime_to_ts(new_inode, current_time(new_inode));
	}

	/* Add entry in new_dir */
	ret = beamfs_add_dirent(new_dir, &new_dentry->d_name,
			       old_inode->i_ino,
			       is_dir ? 4 /* DT_DIR */ : 1 /* DT_REG */);
	if (ret)
		return ret;

	/* Remove entry from old_dir */
	ret = beamfs_del_dirent(old_dir, &old_dentry->d_name);
	if (ret) {
		pr_err("beamfs: rename: del_dirent failed after add, fs may be inconsistent\n");
		return ret;
	}

	/*
	 * Update ".." in the moved directory to point to new_dir.
	 * Also fix nlink on old_dir and new_dir.
	 */
	if (is_dir && old_dir != new_dir) {
		struct qstr dotdot = QSTR_INIT("..", 2);

		ret = beamfs_del_dirent(old_inode, &dotdot);
		if (ret)
			return ret;

		ret = beamfs_add_dirent(old_inode, &dotdot,
				       new_dir->i_ino, 4 /* DT_DIR */);
		if (ret)
			return ret;

		inode_dec_link_count(old_dir);
		inode_inc_link_count(new_dir);

		ret = beamfs_write_inode_raw(old_inode);
		if (ret)
			return ret;
	}

	/* Update timestamps */
	inode_set_ctime_to_ts(old_inode, current_time(old_inode));
	inode_set_mtime_to_ts(old_dir,   current_time(old_dir));
	inode_set_ctime_to_ts(old_dir,   current_time(old_dir));
	inode_set_mtime_to_ts(new_dir,   current_time(new_dir));
	inode_set_ctime_to_ts(new_dir,   current_time(new_dir));

	/* Persist all touched inodes */
	ret = beamfs_write_inode_raw(old_inode);
	if (ret)
		return ret;

	ret = beamfs_write_inode_raw(old_dir);
	if (ret)
		return ret;

	if (old_dir != new_dir) {
		ret = beamfs_write_inode_raw(new_dir);
		if (ret)
			return ret;
	}

	if (new_inode)
		beamfs_write_inode_raw(new_inode);

	return 0;
}

const struct inode_operations beamfs_dir_inode_operations = {
	.lookup  = beamfs_lookup,
	.create  = beamfs_create,
	.mkdir   = beamfs_mkdir,
	.unlink  = beamfs_unlink,
	.rmdir   = beamfs_rmdir,
	.link    = beamfs_link,
	.rename  = beamfs_rename,
};
