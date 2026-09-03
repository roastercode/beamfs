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
#include <linux/fs_dirent.h>
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

	/*
	 * cpu_to_le32, because i_crc32 is __le32 and beamfs_crc32 returns
	 * host order. Assigning one to the other wrote the checksum in
	 * whatever order the CPU used -- invisible on the little-endian
	 * machines this has run on, and a volume no big-endian host could
	 * mount. sparse called it: "incorrect type in assignment".
	 */
	raw->i_crc32 = cpu_to_le32(beamfs_crc32(raw,
				   offsetof(struct beamfs_inode, i_crc32)));

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
/* Helper: directory block resolver (direct + indirect)                */
/* ------------------------------------------------------------------ */

/*
 * beamfs_dir_get_block -- look up or allocate a directory data block.
 *
 * @dir:        directory inode
 * @block_idx:  0-based logical block index. Range 0..(12 + 512 - 1)
 *              = 0..523 = direct (12) + single indirect (512).
 * @alloc:      if true, allocate on demand. If false, lookup only.
 * @out_block:  resolved physical block number on success. Set to 0 if
 *              the block is a HOLE and @alloc is false.
 *
 * Returns:
 *   0    success (*out_block set)
 *   -ENOSPC  no free blocks (when @alloc=true)
 *   -EIO     indirect block read failure
 *   -EINVAL  block_idx >= 524 (caller bug)
 *
 * Direct vs indirect mapping:
 *   block_idx < 12  -> fi->i_direct[block_idx]
 *   block_idx < 524 -> via fi->i_indirect, slot=(block_idx - 12)
 *
 * Newly allocated blocks (both data and indirect) are zero-initialized
 * before being installed. This is required for directory blocks (entry
 * scanners assume d_ino == 0 marks a free slot).
 */
int beamfs_dir_get_block(struct inode *dir, unsigned int block_idx,
			 bool alloc, u64 *out_block)
{
	struct super_block       *sb = dir->i_sb;
	struct beamfs_inode_info *fi = BEAMFS_I(dir);
	struct buffer_head       *ibh;
	struct buffer_head       *dbh;
	__le64                   *ptrs;
	u64                       block_no;
	u64                       indirect_blk;
	u64                       indirect_slot;

	if (!out_block)
		return -EINVAL;
	*out_block = 0;

	if (block_idx >= BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS)
		return -EINVAL;

	/* --- Direct --- */
	if (block_idx < BEAMFS_DIRECT_BLOCKS) {
		block_no = le64_to_cpu(fi->i_direct[block_idx]);
		if (block_no) {
			*out_block = block_no;
			return 0;
		}
		if (!alloc)
			return 0;

		block_no = beamfs_alloc_block(sb, dir);
		if (!block_no)
			return -ENOSPC;
		dbh = sb_getblk(sb, block_no);
		if (!dbh) {
			beamfs_free_block(sb, block_no, dir);
			return -EIO;
		}
		/*
		 * A new directory block is one free record covering the
		 * whole payload, then parity over it.
		 *
		 * Zeroing alone used to be enough because every slot was a
		 * fixed 268 bytes and an all-zero slot read as free. With
		 * variable-length records the walk needs a d_rec_len to
		 * step by, and a block of zeros would stop it at the first
		 * entry -- so the block has to say, once, that all 3824
		 * bytes are available.
		 */
		lock_buffer(dbh);
		memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
		((struct beamfs_dir_entry *)dbh->b_data)->d_rec_len =
			cpu_to_le16(BEAMFS_DATA_INLINE_BYTES);
		beamfs_dirent_encode(dbh);
		set_buffer_uptodate(dbh);
		unlock_buffer(dbh);
		mark_buffer_dirty(dbh);
		brelse(dbh);

		fi->i_direct[block_idx] = cpu_to_le64(block_no);
		dir->i_size += BEAMFS_BLOCK_SIZE;
		mark_inode_dirty(dir);
		*out_block = block_no;
		return 0;
	}

	/* --- Indirect --- */
	indirect_slot = block_idx - BEAMFS_DIRECT_BLOCKS;
	indirect_blk  = le64_to_cpu(fi->i_indirect);

	/* Allocate indirect block on demand if missing. */
	if (!indirect_blk) {
		if (!alloc)
			return 0;
		indirect_blk = beamfs_alloc_block(sb, dir);
		if (!indirect_blk)
			return -ENOSPC;
		ibh = sb_getblk(sb, indirect_blk);
		if (!ibh) {
			beamfs_free_block(sb, indirect_blk, dir);
			return -EIO;
		}
		lock_buffer(ibh);
		memset(ibh->b_data, 0, BEAMFS_BLOCK_SIZE);
		set_buffer_uptodate(ibh);
		unlock_buffer(ibh);
		mark_buffer_dirty(ibh);
		brelse(ibh);
		fi->i_indirect = cpu_to_le64(indirect_blk);
		mark_inode_dirty(dir);
	}

	ibh = sb_bread(sb, indirect_blk);
	if (!ibh)
		return -EIO;
	ptrs = (__le64 *)ibh->b_data;
	block_no = le64_to_cpu(ptrs[indirect_slot]);

	if (block_no) {
		brelse(ibh);
		*out_block = block_no;
		return 0;
	}

	if (!alloc) {
		brelse(ibh);
		return 0;
	}

	block_no = beamfs_alloc_block(sb, dir);
	if (!block_no) {
		brelse(ibh);
		return -ENOSPC;
	}
	dbh = sb_getblk(sb, block_no);
	if (!dbh) {
		beamfs_free_block(sb, block_no, dir);
		brelse(ibh);
		return -EIO;
	}
	/* Same as the direct case: one free record, then parity. */
	lock_buffer(dbh);
	memset(dbh->b_data, 0, BEAMFS_BLOCK_SIZE);
	((struct beamfs_dir_entry *)dbh->b_data)->d_rec_len =
		cpu_to_le16(BEAMFS_DATA_INLINE_BYTES);
	beamfs_dirent_encode(dbh);
	set_buffer_uptodate(dbh);
	unlock_buffer(dbh);
	mark_buffer_dirty(dbh);
	brelse(dbh);

	ptrs[indirect_slot] = cpu_to_le64(block_no);
	mark_buffer_dirty(ibh);
	brelse(ibh);
	dir->i_size += BEAMFS_BLOCK_SIZE;
	mark_inode_dirty(dir);
	*out_block = block_no;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Helper: add a directory entry to a directory inode                  */
/* ------------------------------------------------------------------ */

static int beamfs_add_dirent(struct inode *dir, const struct qstr *name,
			     unsigned long ino, u8 file_type)
{
	struct super_block      *sb = dir->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	u8      *payload;
	u64      block_no;
	u32      offset, place;
	u16      want;
	unsigned int i;
	int      ret = -ENOSPC;

	if (name->len > BEAMFS_MAX_FILENAME)
		return -ENAMETOOLONG;

	want = (u16)BEAMFS_DIRENT_LEN(name->len);

	payload = kmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
	if (!payload)
		return -ENOMEM;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		int rc = beamfs_dir_get_block(dir, i, true, &block_no);

		if (rc) {
			ret = rc;
			goto out;
		}
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh) {
			ret = -EIO;
			goto out;
		}

		rc = beamfs_dirent_decode(sb, bh, payload);
		if (rc < 0) {
			brelse(bh);
			ret = rc;
			goto out;
		}

		/*
		 * Look for a record with room to spare.
		 *
		 * A deleted entry leaves its record in place with d_ino
		 * zeroed, so the space it held is reusable without
		 * compacting the block. An entry in use can also give up
		 * its tail if its record is longer than its name needs --
		 * which is how ext2 has done it since 1993, and what keeps
		 * a directory from fragmenting as names come and go.
		 */
		offset = 0;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u16 rec, used;
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			rec  = le16_to_cpu(de->d_rec_len);
			used = de->d_ino ?
			       (u16)BEAMFS_DIRENT_LEN(de->d_name_len) : 0;

			if (!de->d_ino && rec >= want) {
				/* Free record, large enough as it stands. */
				place = offset;
				goto place_it;
			}
			if (de->d_ino && rec - used >= want &&
			    beamfs_dirent_place(offset + used, want) ==
			    offset + used) {
				/* Split the tail off a live record. */
				de->d_rec_len = cpu_to_le16(used);
				place = offset + used;
				goto place_it;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			offset = next;
		}
		brelse(bh);
		continue;

place_it:
		de = (struct beamfs_dir_entry *)(payload + place);
		{
			u16 rec = le16_to_cpu(de->d_rec_len);

			/*
			 * A record taken from a free slot keeps its old
			 * length so the space is not lost; one split off a
			 * live entry runs to the end of what was there.
			 */
			if (rec < want)
				rec = want;
			memset(de, 0, BEAMFS_DIRENT_HDR_LEN);
			de->d_ino       = cpu_to_le64(ino);
			de->d_rec_len   = cpu_to_le16(rec);
			de->d_name_len  = (u8)name->len;
			de->d_file_type = file_type;
			memcpy(de->d_name, name->name, name->len);
		}

		lock_buffer(bh);
		memcpy(bh->b_data, payload, BEAMFS_DATA_INLINE_BYTES);
		beamfs_dirent_encode(bh);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty(bh);
		brelse(bh);
		ret = 0;
		goto out;
	}

out:
	kfree(payload);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Helper: remove a directory entry from a directory                   */
/* ------------------------------------------------------------------ */

static int beamfs_del_dirent(struct inode *dir, const struct qstr *name)
{
	struct super_block      *sb = dir->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de, *prev;
	u8      *payload;
	u64      block_no;
	u32      offset, prev_off;
	unsigned int i;
	int      ret = -ENOENT;

	payload = kmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
	if (!payload)
		return -ENOMEM;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		int rc = beamfs_dir_get_block(dir, i, false, &block_no);

		if (rc) {
			ret = rc;
			goto out;
		}
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			continue;

		rc = beamfs_dirent_decode(sb, bh, payload);
		if (rc < 0) {
			brelse(bh);
			ret = rc;
			goto out;
		}

		offset = 0;
		prev_off = BEAMFS_DIRENT_NOSPACE;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    de->d_name_len == name->len &&
			    !memcmp(de->d_name, name->name, name->len)) {
				/*
				 * Fold the record into its predecessor when
				 * there is one in the same subblock, so the
				 * space comes back as a single free record
				 * rather than a hole nothing can use.
				 * Otherwise zero d_ino and leave the record,
				 * which add_dirent will find.
				 */
				if (prev_off != BEAMFS_DIRENT_NOSPACE) {
					u16 plen, dlen;

					prev = (struct beamfs_dir_entry *)
						(payload + prev_off);
					plen = le16_to_cpu(prev->d_rec_len);
					dlen = le16_to_cpu(de->d_rec_len);

					if (prev_off / BEAMFS_SUBBLOCK_DATA ==
					    offset / BEAMFS_SUBBLOCK_DATA)
						prev->d_rec_len =
						    cpu_to_le16(plen + dlen);
					else
						de->d_ino = 0;
				} else {
					de->d_ino = 0;
				}

				lock_buffer(bh);
				memcpy(bh->b_data, payload,
				       BEAMFS_DATA_INLINE_BYTES);
				beamfs_dirent_encode(bh);
				set_buffer_uptodate(bh);
				unlock_buffer(bh);
				mark_buffer_dirty(bh);
				brelse(bh);
				ret = 0;
				goto out;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			prev_off = offset;
			offset = next;
		}
		brelse(bh);
	}

out:
	kfree(payload);
	return ret;
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

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino, DT_REG);
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
			       inode->i_ino, DT_DIR);
	if (ret)
		goto out_fail;

	ret = beamfs_add_dirent(inode, &(struct qstr)QSTR_INIT("..", 2),
			       dir->i_ino, DT_DIR);
	if (ret)
		goto out_fail;

	ret = beamfs_write_inode_raw(inode);
	if (ret)
		goto out_fail;

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino,
			       DT_DIR);
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

/*
 * beamfs_dir_is_empty -- does @inode hold anything but . and .. ?
 *
 * i_nlink is not the answer. A regular file does not bump its parent's
 * link count, so a directory full of files still reads nlink == 2 and
 * the obvious test says empty. rmdir made that mistake and could never
 * remove anything; rename made the mirror image of it and silently
 * destroyed the target's contents -- caught by xfstests generic/023,
 * where "dire/tree -> Directory not empty" came back as success.
 *
 * The blocks have to be walked. Holes are skipped rather than treated
 * as the end: a directory with a freed slot followed by live entries
 * is not empty, and stopping at the first hole would say it was.
 */
static int beamfs_dir_is_empty(struct inode *inode)
{
	struct super_block      *sb = inode->i_sb;
	struct buffer_head      *bh;
	struct beamfs_dir_entry *de;
	u8      *payload;
	u64      block_no;
	u32      offset;
	unsigned int i;
	int      ret = 1;

	payload = kmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
	if (!payload)
		return -ENOMEM;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS; i++) {
		int rc = beamfs_dir_get_block(inode, i, false, &block_no);

		if (rc) {
			ret = rc;
			goto out;
		}
		if (!block_no)
			break;

		bh = sb_bread(sb, block_no);
		if (!bh)
			continue;

		rc = beamfs_dirent_decode(sb, bh, payload);
		brelse(bh);
		if (rc < 0) {
			ret = rc;
			goto out;
		}

		offset = 0;
		while (offset < BEAMFS_DATA_INLINE_BYTES) {
			u32 next;

			de = (struct beamfs_dir_entry *)(payload + offset);
			if (!beamfs_dirent_valid(de, offset))
				break;

			if (de->d_ino &&
			    !(de->d_name_len == 1 && de->d_name[0] == '.') &&
			    !(de->d_name_len == 2 && de->d_name[0] == '.' &&
			      de->d_name[1] == '.')) {
				ret = 0;
				goto out;
			}

			next = beamfs_dirent_next(de, offset);
			if (next == BEAMFS_DIRENT_NOSPACE)
				break;
			offset = next;
		}
	}

out:
	kfree(payload);
	return ret;
}

static int beamfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode            *inode = d_inode(dentry);
	int                      ret;

	/* Same walk rename needs; one implementation for both. */
	ret = beamfs_dir_is_empty(inode);
	if (ret)
		return ret;

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

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino,
				fs_umode_to_dtype(inode->i_mode));
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
/* symlink - create a symbolic link                                    */
/* ------------------------------------------------------------------ */

/*
 * beamfs_symlink -- create a symbolic link in directory @dir.
 *
 * Fast symlink only (v1): target path is stored inline in the inode
 * by reusing the i_direct[] byte storage (96 bytes available). For
 * targets longer than 96 bytes, returns -ENAMETOOLONG. The rootfs
 * Linux deployment corpus has all symlinks < 96 bytes; slow symlink
 * (data-block path) is deferred to a follow-up.
 */
/*
 * Put a long target in a data block and point i_direct[0] at it.
 *
 * The block is written through the same RS encoder as file data, so the
 * target is protected exactly as a file of the same length would be.
 * The tail is zeroed rather than left as whatever the allocator
 * returned: a symlink is read by length, but an uninitialised tail
 * still means the block's parity covers bytes nobody chose.
 */
static int beamfs_symlink_store_block(struct inode *inode, const char *target,
				      size_t len)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	u64 phys;
	u8 *staging;
	int ret = 0;

	staging = kzalloc(BEAMFS_DATA_INLINE_BYTES, GFP_NOFS);
	if (!staging)
		return -ENOMEM;
	memcpy(staging, target, len);

	phys = beamfs_alloc_block(sb, inode);
	if (!phys) {
		kfree(staging);
		return -ENOSPC;
	}

	bh = sb_getblk(sb, phys);
	if (!bh) {
		beamfs_free_block(sb, phys, inode);
		kfree(staging);
		return -EIO;
	}

	lock_buffer(bh);
	memset(bh->b_data, 0, BEAMFS_BLOCK_SIZE);
	for (unsigned int i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy((u8 *)bh->b_data + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       staging + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
	beamfs_rs_encode_region((u8 *)bh->b_data, BEAMFS_SUBBLOCK_TOTAL,
				(u8 *)bh->b_data + BEAMFS_SUBBLOCK_DATA,
				BEAMFS_SUBBLOCK_TOTAL,
				BEAMFS_SUBBLOCK_DATA,
				BEAMFS_DATA_INLINE_SUBBLOCKS);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
	ret = sync_dirty_buffer(bh);
	brelse(bh);
	kfree(staging);

	if (ret) {
		beamfs_free_block(sb, phys, inode);
		return ret;
	}

	fi->i_direct[0] = cpu_to_le64(phys);
	return 0;
}

static int beamfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, const char *symname)
{
	struct inode             *inode;
	struct beamfs_inode_info *fi;
	size_t                    len;
	int                       ret;

	len = strlen(symname);
	/*
	 * PATH_MAX is the ceiling the VFS enforces; anything under it is a
	 * legitimate target and has to be storable. Ninety-six bytes was
	 * not a design limit, it was the size of the field the short form
	 * happens to reuse, and any absolute path of moderate depth
	 * exceeds it -- generic/360 links to a 1019-byte path and got
	 * ENAMETOOLONG.
	 */
	if (len == 0 || len > BEAMFS_DATA_INLINE_BYTES)
		return -ENAMETOOLONG;

	inode = beamfs_new_inode(dir, S_IFLNK | 0777);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	fi = BEAMFS_I(inode);
	memset(fi->i_direct, 0, sizeof(fi->i_direct));

	if (len < sizeof(fi->i_direct)) {
		/*
		 * Short form: the target lives in i_direct[] as raw bytes,
		 * carried to disk by write_inode_raw's verbatim memcpy and
		 * covered by the inode's own CRC and RS parity.
		 */
		memcpy(fi->i_direct, symname, len);
	} else {
		/*
		 * Long form: the target goes in a data block, and
		 * i_direct[0] holds its number. i_size tells the two apart
		 * on the way back, so nothing new is needed on disk.
		 *
		 * A block brings the RS codewords with it, so a long target
		 * ends up better protected than a short one -- which is the
		 * right way round, since it carries more to lose.
		 */
		ret = beamfs_symlink_store_block(inode, symname, len);
		if (ret)
			goto out_iput;
	}

	inode->i_size = len;
	inode->i_op = &beamfs_symlink_inode_operations;

	ret = beamfs_write_inode_raw(inode);
	if (ret)
		goto out_iput;

	ret = beamfs_add_dirent(dir, &dentry->d_name, inode->i_ino, DT_LNK);
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

/*
 * beamfs_get_link -- VFS i_op->get_link callback.
 *
 * Returns a pointer to the inline target stored in i_direct[]. The
 * buffer lifetime is tied to the in-memory inode; no allocation is
 * performed and no DELAYED_CALL needs to be set up.
 */
static const char *beamfs_get_link(struct dentry *dentry,
				   struct inode *inode,
				   struct delayed_call *done)
{
	struct beamfs_inode_info *fi = BEAMFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	loff_t len = i_size_read(inode);
	u64 phys;
	char *buf;
	int ret;

	if (len < (loff_t)sizeof(fi->i_direct))
		return (const char *)fi->i_direct;

	/*
	 * Long form. RCU-walk cannot take the buffer lock or sleep on a
	 * read, so hand it back to the caller for a ref-walk retry.
	 */
	if (!dentry)
		return ERR_PTR(-ECHILD);

	if (len > BEAMFS_DATA_INLINE_BYTES)
		return ERR_PTR(-EUCLEAN);

	phys = le64_to_cpu(fi->i_direct[0]);
	if (!phys)
		return ERR_PTR(-EUCLEAN);

	buf = kzalloc(BEAMFS_DATA_INLINE_BYTES + 1, GFP_NOFS);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	bh = sb_bread(sb, phys);
	if (!bh) {
		kfree(buf);
		return ERR_PTR(-EIO);
	}

	lock_buffer(bh);
	ret = beamfs_inline_decode_symlink(sb, bh, phys, inode, buf,
					   (u32)len);
	unlock_buffer(bh);
	brelse(bh);

	if (ret) {
		kfree(buf);
		return ERR_PTR(ret);
	}

	buf[len] = '\0';
	set_delayed_call(done, kfree_link, buf);
	return buf;
}

const struct inode_operations beamfs_symlink_inode_operations = {
	.getattr  = simple_getattr,
	.get_link = beamfs_get_link,
};

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
			ret = beamfs_dir_is_empty(new_inode);
			if (ret)
				return ret;
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
			       is_dir ? DT_DIR : DT_REG);
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
				       new_dir->i_ino, DT_DIR);
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
	.symlink = beamfs_symlink,
	.rename  = beamfs_rename,
};
