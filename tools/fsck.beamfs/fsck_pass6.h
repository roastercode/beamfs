/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * pass 6 -- directories, link counts, and who owns which block.
 *
 * The five passes that existed all reason about blocks and bits. This
 * one reasons about the filesystem: that the root is a directory, that
 * every entry names an inode that exists, that link counts match the
 * entries pointing at them, that every allocated inode is reachable,
 * that no block belongs to two files, and that no file claims more
 * bytes than its blocks hold.
 *
 * Shared blocks are the reason this exists. Two inodes pointing at one
 * block means writing to one file silently rewrites another, and
 * nothing was looking for it -- on a filesystem where the defect under
 * investigation could produce exactly that.
 */

#ifndef BEAMFS_FSCK_PASS6_H
#define BEAMFS_FSCK_PASS6_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "fsck_read.h"

/*
 * Names remembered per directory, for duplicate detection.
 *
 * Bounded on purpose: a directory of a million entries would otherwise
 * need a megabyte of names to check something that has never gone
 * wrong in practice, and a checker that runs out of memory checks
 * nothing at all. Beyond this, duplicates in that directory go
 * unreported rather than the pass failing.
 */
#define FSCK_PASS6_MAX_NAMES 4096

enum {
	FSCK_PASS6_OK          = 0,
	FSCK_PASS6_UNCORRECTED = 1,
	FSCK_PASS6_ERROR       = 2,
};

struct fsck_pass6_opts {
	bool verbose;
	bool repair;
};

struct fsck_pass6_result {
	unsigned int allocated_inodes;

	/* Two inodes claiming one block. */
	unsigned int shared_blocks;
	/* The root is missing, free, or not a directory. */
	bool         root_bad;
	/* Record lengths that do not tile the block. */
	unsigned int bad_dirents;
	/* Entries naming an inode that is not allocated. */
	unsigned int dangling_entries;
	unsigned int duplicate_names;
	/* i_nlink against the entries actually found. */
	unsigned int link_count_wrong;
	/* Allocated, but no directory reaches it. */
	unsigned int orphaned_inodes;
	/* i_size larger than the blocks the inode owns. */
	unsigned int size_beyond_blocks;

	/* Left unwalked rather than followed into nonsense. */
	unsigned int unreadable_inodes;
	unsigned int unreadable_indirect;
	unsigned int unreadable_dirblocks;
	/* Directories too deep for this walk's single name table. */
	unsigned int deep_directories;
};

int  fsck_pass6(struct fsck_reader *rd, const struct fsck_pass6_opts *o,
		struct fsck_pass6_result *out);
void fsck_pass6_report(const struct fsck_pass6_result *r);

#endif /* BEAMFS_FSCK_PASS6_H */
