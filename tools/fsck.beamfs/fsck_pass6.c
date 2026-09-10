// SPDX-License-Identifier: GPL-2.0-only
/*
 * pass 6 -- what the checker was not looking for.
 *
 * fsck.beamfs checked five things: the superblock, the bitmap, that
 * each inode's pointers are in range, that the bitmap agrees with the
 * inode table, and the RS journal. Every one of those is about a block
 * or a bit. Nothing looked at the filesystem as a filesystem.
 *
 * e2fsck spends two of its five passes on directories alone, for
 * reasons that apply here identically. What follows is the set of
 * checks that were missing, in the order that damage tends to matter:
 *
 *   shared blocks     two inodes pointing at one block. The most
 *                     destructive corruption there is -- writing to
 *                     one file silently rewrites another -- and
 *                     nothing was looking. It is also what the defect
 *                     under investigation could produce, so its
 *                     absence has been shaping two days of analysis.
 *
 *   the root          inode 1 must exist and be a directory. Without
 *                     it nothing is reachable and every other check
 *                     reports on a filesystem nobody can mount.
 *
 *   directory entries every entry must name an allocated inode, the
 *                     records must tile the block exactly, and no name
 *                     may repeat within one directory.
 *
 *   link counts       i_nlink must equal the number of entries
 *                     pointing at the inode. Too high and the file is
 *                     never freed; too low and it is freed while in
 *                     use, which is how one file's blocks end up in
 *                     another.
 *
 *   connectivity      an allocated inode no directory reaches is
 *                     orphaned: its blocks are accounted for, so no
 *                     leak is reported, and the space is unusable
 *                     until someone notices.
 *
 *   size against      i_size must fit the blocks the inode owns. A
 *   allocation        file claiming more than it holds reads past its
 *                     own data.
 *
 * Everything here reads through fsck_read, so it sees corrected data
 * like every other pass and cannot be misled by one recoverable bit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beamfs_format.h"
#include "fsck_read.h"
#include "fsck_pass6.h"

/* Inode 1 is the root, by the same convention ext2 uses. */
#define ROOT_INO 1

#define S_FMT   0xF000
#define S_DIR   0x4000
#define S_REG   0x8000
#define S_LNK   0xA000

struct p6 {
	struct fsck_reader *rd;
	const struct fsck_pass6_opts *o;

	/* Who owns each data block, 0 for nobody. Sized for the volume;
	 * 8 bytes a block is 2 MiB on a 1 GiB volume, which is worth it
	 * to name both inodes in a sharing report rather than saying
	 * only that sharing exists. */
	uint64_t *owner;

	/* Counted references per inode, to compare against i_nlink. */
	uint32_t *seen_links;

	/* Which inodes a directory walk reached. */
	uint8_t  *reachable;

	uint64_t  nblocks, ninodes;

	struct fsck_pass6_result r;
};

static void note(struct p6 *p, const char *fmt, ...)
{
	va_list ap;

	if (!p->o->verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/*
 * Claim a block for an inode, reporting a second claimant.
 *
 * The first owner is kept and the second reported: which of the two is
 * the impostor cannot be decided here, and guessing would be worse than
 * naming both.
 */
static void claim(struct p6 *p, uint64_t phys, uint64_t ino)
{
	uint64_t idx;

	/* Only blocks inside the allocation region can be shared in a
	 * way that matters: the root and the canary sit below
	 * data_start, belong to exactly one inode by construction, and
	 * have no bitmap entry to contend over. */
	if (phys < p->rd->data_start || phys >= p->rd->data_start + p->nblocks)
		return;
	idx = phys - p->rd->data_start;

	if (p->owner[idx] != 0 && p->owner[idx] != ino) {
		p->r.shared_blocks++;
		note(p, "fsck.beamfs: pass 6: block %llu claimed by inode %llu and inode %llu\n",
		     (unsigned long long)phys,
		     (unsigned long long)p->owner[idx],
		     (unsigned long long)ino);
		return;
	}
	p->owner[idx] = ino;
}

/*
 * Walk an indirect subtree, claiming every block for @ino.
 *
 * Returns how many blocks the subtree holds, itself included. The count
 * is the point as much as the claiming: without it an inode's block
 * total stopped at twelve direct and one indirect, so every file past
 * 45 KiB looked like it claimed more bytes than its blocks could hold.
 * On a Yocto rootfs that was 1426 reports, none of them real.
 */
static uint64_t claim_tree(struct p6 *p, uint64_t blk, int level, uint64_t ino)
{
	uint64_t ptrs[BEAMFS_INDIRECT_PTRS];
	uint64_t n = 1;
	unsigned int i;

	if (blk == 0)
		return 0;
	claim(p, blk, ino);

	if (fsck_read_indirect(p->rd, blk, ptrs) == FSCK_READ_UNCORRECTABLE) {
		/* Not walked: following pointers out of a block that
		 * failed its own parity is how one bad block becomes a
		 * report of hundreds. */
		p->r.unreadable_indirect++;
		return n;
	}
	for (i = 0; i < BEAMFS_INDIRECT_PTRS; i++) {
		if (ptrs[i] == 0)
			continue;
		if (level > 1) {
			n += claim_tree(p, ptrs[i], level - 1, ino);
		} else {
			claim(p, ptrs[i], ino);
			n++;
		}
	}
	return n;
}

/* How many blocks an inode actually owns, and its size in blocks. */
static uint64_t count_blocks(struct p6 *p, const struct beamfs_inode *in,
			     uint64_t ino)
{
	uint64_t n = 0;
	unsigned int i;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++)
		if (in->i_direct[i]) {
			claim(p, le64toh(in->i_direct[i]), ino);
			n++;
		}
	/* The subtree's own count, not one for the root of it: an
	 * indirect block holding fifteen data pointers is sixteen
	 * blocks, and counting it as one is what made every large file
	 * look short of blocks. */
	n += claim_tree(p, le64toh(in->i_indirect), 1, ino);
	n += claim_tree(p, le64toh(in->i_dindirect), 2, ino);
	n += claim_tree(p, le64toh(in->i_tindirect), 3, ino);
	return n;
}

/*
 * Read one directory block and visit its entries.
 *
 * The records are laid out contiguously in the first
 * BEAMFS_DATA_INLINE_BYTES, the way the data path lays out a block, so
 * the parity slots are not in the way of the walk.
 */
static void walk_dir_block(struct p6 *p, uint64_t blk, uint64_t dir_ino,
			   char (*names)[BEAMFS_MAX_FILENAME + 1], unsigned int *nnames)
{
	uint8_t flat[BEAMFS_DATA_INLINE_BYTES];
	uint32_t off = 0;

	if (fsck_read_data(p->rd, blk, flat) == FSCK_READ_UNCORRECTABLE) {
		p->r.unreadable_dirblocks++;
		return;
	}

	while (off + BEAMFS_DIRENT_HDR_LEN <= BEAMFS_DATA_INLINE_BYTES) {
		const struct beamfs_dir_entry *de =
			(const struct beamfs_dir_entry *)(flat + off);
		uint64_t ino = le64toh(de->d_ino);
		uint16_t rec = le16toh(de->d_rec_len);
		uint8_t  nl  = de->d_name_len;
		unsigned int k;

		/*
		 * Zero is the end of the block, not damage.
		 *
		 * mkfs writes a trailing free record on the last block of
		 * a directory and leaves the intermediate ones ending in
		 * zeroes, and the kernel's walk stops there --
		 * beamfs_dirent_valid returns false and the loop breaks.
		 * A checker stricter than the reader reports fifty-three
		 * malformed records on a healthy Yocto rootfs, which is
		 * what this did.
		 *
		 * A length that is non-zero and still impossible is
		 * another matter: it would step into the middle of the
		 * next record, and the walk cannot continue past it.
		 */
		if (rec == 0)
			return;

		if (rec < BEAMFS_DIRENT_MIN_LEN || (rec & (BEAMFS_DIRENT_ALIGN - 1)) ||
		    off + rec > BEAMFS_DATA_INLINE_BYTES) {
			p->r.bad_dirents++;
			note(p, "fsck.beamfs: pass 6: directory %llu block %llu: record length %u at offset %u cannot be stepped over\n",
			     (unsigned long long)dir_ino, (unsigned long long)blk,
			     rec, off);
			return;
		}

		if (ino != 0) {
			if (ino > p->ninodes) {
				p->r.bad_dirents++;
				note(p, "fsck.beamfs: pass 6: directory %llu names inode %llu, past the end of the table\n",
				     (unsigned long long)dir_ino,
				     (unsigned long long)ino);
				/* d_name_len is a byte and BEAMFS_MAX_FILENAME is
				 * 255, so it cannot exceed it -- what can go wrong
				 * is a name that does not fit the record it sits
				 * in, which is the check that matters. */
			} else if (nl == 0 ||
				   BEAMFS_DIRENT_HDR_LEN + nl > rec) {
				p->r.bad_dirents++;
				note(p, "fsck.beamfs: pass 6: directory %llu has an entry with a name of %u bytes in a %u-byte record\n",
				     (unsigned long long)dir_ino, nl, rec);
			} else {
				struct beamfs_inode target;

				/* Duplicate names in one directory: two
				 * entries, one file, and a rename or an
				 * unlink then works on whichever is
				 * found first. */
				if (*nnames < FSCK_PASS6_MAX_NAMES) {
					for (k = 0; k < *nnames; k++)
						if (strncmp(names[k], de->d_name, nl) == 0 &&
						    names[k][nl] == '\0') {
							p->r.duplicate_names++;
							note(p, "fsck.beamfs: pass 6: directory %llu has two entries named %.*s\n",
							     (unsigned long long)dir_ino,
							     nl, de->d_name);
							break;
						}
					if (k == *nnames) {
						memcpy(names[*nnames], de->d_name, nl);
						names[*nnames][nl] = '\0';
						(*nnames)++;
					}
				}

				if (fsck_read_inode(p->rd, ino, &target)
				    != FSCK_READ_UNCORRECTABLE) {
					if (target.i_mode == 0) {
						p->r.dangling_entries++;
						note(p, "fsck.beamfs: pass 6: directory %llu names inode %llu, which is free\n",
						     (unsigned long long)dir_ino,
						     (unsigned long long)ino);
					} else {
						p->seen_links[ino]++;
						p->reachable[ino / 8] |=
							(uint8_t)(1u << (ino % 8));
					}
				}
			}
		}
		off += rec;
	}
}

/* Every block of a directory, direct and indirect alike. */
static void walk_directory(struct p6 *p, uint64_t ino,
			   const struct beamfs_inode *in)
{
	static char names[FSCK_PASS6_MAX_NAMES][BEAMFS_MAX_FILENAME + 1];
	unsigned int nnames = 0;
	unsigned int i;

	for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++)
		if (in->i_direct[i])
			walk_dir_block(p, in->i_direct[i], ino, names, &nnames);

	if (in->i_indirect) {
		uint64_t ptrs[BEAMFS_INDIRECT_PTRS];

		if (fsck_read_indirect(p->rd, in->i_indirect, ptrs)
		    != FSCK_READ_UNCORRECTABLE) {
			for (i = 0; i < BEAMFS_INDIRECT_PTRS; i++)
				if (ptrs[i])
					walk_dir_block(p, ptrs[i], ino, names, &nnames);
		}
	}
	/* Directories deep enough to need double indirection do not
	 * occur in practice and walking them would need a second name
	 * table; they are counted so their absence is not silent. */
	if (in->i_dindirect || in->i_tindirect)
		p->r.deep_directories++;
}

int fsck_pass6(struct fsck_reader *rd, const struct fsck_pass6_opts *o,
	       struct fsck_pass6_result *out)
{
	struct p6 p;
	struct beamfs_inode in;
	uint64_t ino;
	int rc = FSCK_PASS6_OK;

	memset(&p, 0, sizeof(p));
	p.rd = rd;
	p.o = o;
	p.nblocks = rd->nblocks;
	p.ninodes = rd->inode_count;

	p.owner      = calloc(p.nblocks, sizeof(uint64_t));
	p.seen_links = calloc(p.ninodes + 1, sizeof(uint32_t));
	p.reachable  = calloc((p.ninodes + 8) / 8, 1);
	if (!p.owner || !p.seen_links || !p.reachable) {
		free(p.owner);
		free(p.seen_links);
		free(p.reachable);
		fprintf(stderr, "fsck.beamfs: pass 6: out of memory\n");
		return FSCK_PASS6_ERROR;
	}

	/*
	 * The root, before anything else.
	 *
	 * Every reachability answer below is relative to it. A checker
	 * that reports every inode as orphaned because the root is
	 * missing has said one thing badly instead of the one thing
	 * that matters.
	 */
	if (fsck_read_inode(rd, ROOT_INO, &in) == FSCK_READ_UNCORRECTABLE) {
		p.r.root_bad = true;
		fprintf(stderr, "fsck.beamfs: pass 6: root inode unreadable\n");
	} else if (in.i_mode == 0) {
		p.r.root_bad = true;
		fprintf(stderr, "fsck.beamfs: pass 6: root inode is not allocated\n");
	} else if ((le16toh(in.i_mode) & S_FMT) != S_DIR) {
		p.r.root_bad = true;
		fprintf(stderr, "fsck.beamfs: pass 6: root inode is not a directory (mode 0%o)\n",
			le16toh(in.i_mode));
	} else {
		p.reachable[ROOT_INO / 8] |= (uint8_t)(1u << (ROOT_INO % 8));
		p.seen_links[ROOT_INO]++;   /* its own "." */
	}

	for (ino = 1; ino <= p.ninodes; ino++) {
		uint16_t mode;
		uint64_t owned, need;

		if (fsck_read_inode(rd, ino, &in) == FSCK_READ_UNCORRECTABLE) {
			p.r.unreadable_inodes++;
			continue;
		}
		if (in.i_mode == 0)
			continue;

		p.r.allocated_inodes++;
		mode = le16toh(in.i_mode);

		/* Claims every block, and reports a second claimant. */
		owned = count_blocks(&p, &in, ino);

		/*
		 * Size against allocation, reported and not counted.
		 *
		 * A file being written has i_size ahead of its blocks for
		 * as long as the writeback has not caught up, and that is
		 * an ordinary state rather than damage: generic/464
		 * unmounts mid-write on purpose, and treating the gap as
		 * an inconsistency failed three trials in five against a
		 * filesystem that was doing exactly what it should.
		 *
		 * A sparse file is the same shape for a different reason
		 * -- a hole occupies no block and i_size counts it -- and
		 * neither can be told from a truncated allocation by
		 * looking at the inode. So it is worth saying out loud
		 * under -v and worth nothing as a verdict.
		 *
		 * A symlink stores its target inside the inode when it
		 * fits, so its size says nothing about blocks at all.
		 */
		if ((mode & S_FMT) != S_LNK) {
			need = (le64toh(in.i_size) + BEAMFS_DATA_INLINE_BYTES - 1)
			     / BEAMFS_DATA_INLINE_BYTES;
			if (need > owned) {
				p.r.size_ahead_of_blocks++;
				note(&p, "fsck.beamfs: pass 6: inode %llu claims %llu bytes, needing %llu block(s), but owns %llu\n",
				     (unsigned long long)ino,
				     (unsigned long long)le64toh(in.i_size),
				     (unsigned long long)need,
				     (unsigned long long)owned);
			}
		}

		if ((mode & S_FMT) == S_DIR)
			walk_directory(&p, ino, &in);
	}

	/* Link counts and reachability, once every directory has been
	 * seen: an inode's entries may live in a directory walked after
	 * it. */
	for (ino = 1; ino <= p.ninodes; ino++) {
		uint16_t mode;

		if (fsck_read_inode(rd, ino, &in) == FSCK_READ_UNCORRECTABLE)
			continue;
		if (in.i_mode == 0)
			continue;
		mode = le16toh(in.i_mode);

		if (!(p.reachable[ino / 8] & (1u << (ino % 8)))) {
			p.r.orphaned_inodes++;
			note(&p, "fsck.beamfs: pass 6: inode %llu is allocated but no directory reaches it\n",
			     (unsigned long long)ino);
			continue;
		}

		/*
		 * Directories carry a link for "." and one from their
		 * parent, plus one per subdirectory's "..", and this
		 * walk does not descend into "." and ".." as entries.
		 * Counting them exactly would need the subdirectory
		 * census; what is checked is the direction that causes
		 * harm -- an inode with more links recorded than
		 * entries pointing at it never gets freed.
		 */
		if ((mode & S_FMT) != S_DIR &&
		    le16toh(in.i_nlink) != p.seen_links[ino]) {
			p.r.link_count_wrong++;
			note(&p, "fsck.beamfs: pass 6: inode %llu records %u link(s), %u entr%s point at it\n",
			     (unsigned long long)ino, le16toh(in.i_nlink),
			     p.seen_links[ino],
			     p.seen_links[ino] == 1 ? "y" : "ies");
		}
	}

	free(p.owner);
	free(p.seen_links);
	free(p.reachable);

	*out = p.r;

	if (p.r.root_bad || p.r.shared_blocks)
		rc = FSCK_PASS6_UNCORRECTED;
	else if (p.r.bad_dirents || p.r.dangling_entries || p.r.duplicate_names ||
		 p.r.link_count_wrong || p.r.orphaned_inodes)
		rc = FSCK_PASS6_UNCORRECTED;
	return rc;
}

void fsck_pass6_report(const struct fsck_pass6_result *r)
{
	if (r->shared_blocks)
		fprintf(stderr, "fsck.beamfs: pass 6: %u block(s) claimed by more than one inode -- writing to one file rewrites another\n",
			r->shared_blocks);
	if (r->root_bad)
		fprintf(stderr, "fsck.beamfs: pass 6: the root directory is unusable; nothing is reachable\n");
	if (r->bad_dirents)
		fprintf(stderr, "fsck.beamfs: pass 6: %u malformed directory record(s)\n",
			r->bad_dirents);
	if (r->dangling_entries)
		fprintf(stderr, "fsck.beamfs: pass 6: %u directory entr%s naming a free inode\n",
			r->dangling_entries, r->dangling_entries == 1 ? "y" : "ies");
	if (r->duplicate_names)
		fprintf(stderr, "fsck.beamfs: pass 6: %u duplicate name(s) within a directory\n",
			r->duplicate_names);
	if (r->link_count_wrong)
		fprintf(stderr, "fsck.beamfs: pass 6: %u inode(s) whose link count does not match the entries pointing at them\n",
			r->link_count_wrong);
	if (r->orphaned_inodes)
		fprintf(stderr, "fsck.beamfs: pass 6: %u allocated inode(s) no directory reaches\n",
			r->orphaned_inodes);
	if (r->size_ahead_of_blocks)
		fprintf(stderr, "fsck.beamfs: pass 6: %u inode(s) whose size runs ahead of their blocks -- ordinary for a file being written or a sparse one, not counted as damage\n",
			r->size_ahead_of_blocks);
	if (r->unreadable_indirect || r->unreadable_dirblocks || r->unreadable_inodes)
		fprintf(stderr, "fsck.beamfs: pass 6: %u inode(s), %u indirect block(s) and %u directory block(s) were beyond correction and left unwalked\n",
			r->unreadable_inodes, r->unreadable_indirect,
			r->unreadable_dirblocks);
	if (r->deep_directories)
		fprintf(stderr, "fsck.beamfs: pass 6: %u directory/ies use double or triple indirection and were only partly walked\n",
			r->deep_directories);

	if (!r->shared_blocks && !r->root_bad && !r->bad_dirents &&
	    !r->dangling_entries && !r->duplicate_names && !r->link_count_wrong &&
	    !r->orphaned_inodes)
		printf("fsck.beamfs: pass 6: directories, links and block ownership OK (%u inode(s))\n",
		       r->allocated_inodes);
}
