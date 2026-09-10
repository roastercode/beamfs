// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck.beamfs - Offline filesystem checker for beamfs
 *
 * Position: complements the in-kernel online Reed-Solomon recovery.
 * Operates on the block device directly when the kernel cannot
 * (post-FS_PANIC, pre-mount validation, post-event audit).
 * See Documentation/fsck.beamfs.md for the full design.
 *
 * Sub-phase 1: skeleton -- argument parsing, exit codes, pass stubs.
 * Sub-phases 2-6 will implement the 5 passes (SB, bitmap, inode
 * walk, bitmap rebuild, RS journal validation).
 *
 * Mainline conformance contract (see fsck.beamfs.md sec 4):
 *   - C, POSIX, libc only (no external libraries);
 *   - exit codes follow fsck(8) convention;
 *   - errors on stderr, progress on stdout only with --verbose;
 *   - manpage fsck.beamfs.8 ships with the binary (sub-phase 7).
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "crc32.h"
#include "rs_decode.h"
#include "fsck_read.h"
#include "fsck_pass6.h"
#include "sb_layout.h"

/*
 * Exit codes per fsck(8) convention.
 * See Documentation/fsck.beamfs.md section 3.1 for the contract.
 */
#define FSCK_OK            0  /* No errors */
#define FSCK_CORRECTED     1  /* Errors corrected (after --repair) */
#define FSCK_REBOOT        2  /* System should reboot (unused) */
#define FSCK_UNCORRECTED   4  /* Errors left uncorrected */
#define FSCK_ERROR         8  /* Operational error (cannot open, etc.) */
#define FSCK_USAGE        16  /* Usage or syntax error */
#define FSCK_CANCELED     32  /* fsck canceled by user */
#define FSCK_LIB         128  /* Shared library error */

#define FSCK_BEAMFS_VERSION "0.1.0"

struct fsck_opts {
	const char *device;
	bool        check_only;
	bool        repair;
	bool        force;
	bool        verbose;
	int         fd;
	/*
	 * Set by pass1_superblock(). false means the superblock fields
	 * used by later passes (s_bitmap_blk, s_data_start_blk, ...) are
	 * not known-good -- a bad magic without --force, or RS-uncorrectable
	 * region A/B. Passes 2-5 must not run in that state: they would
	 * read block offsets derived from values that failed validation.
	 * true (the default; also true after a repaired or already-clean
	 * superblock) means later passes may proceed.
	 */
	bool        sb_trustworthy;
};

static void print_usage(FILE *stream, const char *prog)
{
	fprintf(stream,
		"Usage: %s [OPTIONS] <device>\n"
		"\n"
		"Offline filesystem checker for beamfs.\n"
		"\n"
		"Options:\n"
		"  -n, --check-only   Read-only check, no modification (default)\n"
		"  -p, --repair       Repair correctable errors\n"
		"  -y, --yes          Same as --repair (fsck(8) convention)\n"
		"  -f, --force        Bypass early-abort sanity checks\n"
		"  -v, --verbose      Print progress to stdout\n"
		"  -V, --version      Print version and exit\n"
		"  -h, --help         Print this help and exit\n"
		"\n"
		"Exit codes follow fsck(8):\n"
		"   0  no errors\n"
		"   1  errors corrected\n"
		"   4  errors left uncorrected\n"
		"   8  operational error\n"
		"  16  usage or syntax error\n"
		"  32  canceled by user\n"
		" 128  shared library error\n",
		prog);
}

static void version(void)
{
	printf("fsck.beamfs %s\n", FSCK_BEAMFS_VERSION);
}

/*
 * Stubs for the 5 passes. Each one returns FSCK_OK in sub-phase 1
 * and logs to stderr that it is not yet implemented. Real
 * implementations land in sub-phases 2-6.
 */
/*
 * pass1_superblock -- read, RS-decode and CRC-validate block 0.
 *
 * Order matters: RS-decode runs first, over the 13 shortened
 * RS(255,239) subblocks covering [0, off_crc32) + [off_uuid, off_pad),
 * exactly as mkfs.beamfs::sb_to_rs_staging lays them out. s_magic sits
 * inside that coverage (offset 0), so a corrupted magic byte is a
 * candidate for RS correction like any other covered byte, not a
 * reason to abort before attempting one.
 *
 * s_crc32 itself is deliberately outside RS coverage (same doctrine
 * as the DATA_CSUM descriptor, format-v6.md section 3.3): a flip on
 * s_crc32 can only produce a false mismatch against data that RS has
 * already proven correct, never mask a real corruption of the data
 * RS decoded. So RS-clean + CRC-mismatch means the checksum field
 * itself is wrong, not the superblock content; --repair restates
 * s_crc32 from the computed value rather than treating it as a data
 * loss event.
 *
 * check-only reports a correctable finding as FSCK_UNCORRECTED, not
 * FSCK_OK: an in-memory correction that is never written back is not
 * something that happened to the volume, and silently returning OK
 * would hide a real (if repairable) error from the operator.
 */
static int pass1_superblock(struct fsck_opts *o)
{
	struct beamfs_super_block sb;
	uint8_t   staging[BEAMFS_SB_RS_STAGING_BYTES];
	uint8_t  *parity = (uint8_t *)&sb + BEAMFS_SB_RS_PARITY_OFFSET;
	struct rs_codec *rs;
	ssize_t   got;
	unsigned int i;
	unsigned int total_corrected = 0;
	unsigned int uncorrectable = 0;
	int       positions[RS_NROOTS / 2];
	uint32_t  computed_crc;

	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);

	if (lseek(o->fd, 0, SEEK_SET) < 0) {
		fprintf(stderr, "fsck.beamfs: pass 1: lseek: %s\n",
			strerror(errno));
		return FSCK_ERROR;
	}
	got = read(o->fd, &sb, sizeof(sb));
	if (got != (ssize_t)sizeof(sb)) {
		fprintf(stderr, "fsck.beamfs: pass 1: short read on block 0 (%zd/%zu)\n",
			got, sizeof(sb));
		return FSCK_ERROR;
	}

	rs = rs_init();
	if (!rs) {
		fprintf(stderr, "fsck.beamfs: pass 1: rs_init failed\n");
		return FSCK_ERROR;
	}

	memcpy(staging, &sb, off_crc32);
	memcpy(staging + off_crc32, (uint8_t *)&sb + off_uuid, off_pad - off_uuid);
	memset(staging + BEAMFS_SB_RS_COVERAGE_BYTES, 0,
	       sizeof(staging) - BEAMFS_SB_RS_COVERAGE_BYTES);

	for (i = 0; i < BEAMFS_SB_RS_SUBBLOCKS; i++) {
		int rc = rs_decode_subblock(rs,
					    staging + i * BEAMFS_SB_RS_DATA_LEN,
					    BEAMFS_SB_RS_DATA_LEN,
					    parity + i * RS_NROOTS,
					    positions);
		if (rc == RS_UNCORRECTABLE) {
			uncorrectable++;
			if (o->verbose)
				fprintf(stderr, "fsck.beamfs: pass 1: subblock %u uncorrectable\n", i);
		} else if (rc > 0) {
			total_corrected += (unsigned int)rc;
			if (o->verbose)
				fprintf(stderr, "fsck.beamfs: pass 1: subblock %u: %d symbol(s) corrected\n", i, rc);
		}
	}
	rs_free(rs);

	if (uncorrectable > 0) {
		fprintf(stderr, "fsck.beamfs: pass 1: superblock RS-uncorrectable (%u/%u subblocks)\n",
			uncorrectable, BEAMFS_SB_RS_SUBBLOCKS);
		o->sb_trustworthy = false;
		return FSCK_UNCORRECTED;
	}

	if (total_corrected > 0) {
		/* Parity bytes were already corrected in place: parity
		 * pointed directly into sb.s_pad, so rs_decode_subblock's
		 * in-place correction applied to sb itself, not a copy.
		 */
		memcpy(&sb, staging, off_crc32);
		memcpy((uint8_t *)&sb + off_uuid, staging + off_crc32, off_pad - off_uuid);
	}

	if (sb.s_magic != BEAMFS_MAGIC) {
		if (!o->force) {
			fprintf(stderr, "fsck.beamfs: pass 1: bad magic 0x%08x (expected 0x%08x); use --force to proceed\n",
				sb.s_magic, BEAMFS_MAGIC);
			o->sb_trustworthy = false;
			return FSCK_UNCORRECTED;
		}
		fprintf(stderr, "fsck.beamfs: pass 1: bad magic 0x%08x, continuing (--force)\n",
			sb.s_magic);
	}

	computed_crc = crc32_sb(&sb);
	if (computed_crc != sb.s_crc32) {
		if (total_corrected > 0) {
			fprintf(stderr, "fsck.beamfs: pass 1: s_crc32 stale after RS correction (have 0x%08x, want 0x%08x)\n",
				sb.s_crc32, computed_crc);
		} else {
			fprintf(stderr, "fsck.beamfs: pass 1: s_crc32 mismatch on RS-clean block (have 0x%08x, want 0x%08x)\n",
				sb.s_crc32, computed_crc);
		}
		if (o->repair) {
			sb.s_crc32 = computed_crc;
			total_corrected++;
		} else {
			return FSCK_UNCORRECTED;
		}
	}

	if (total_corrected > 0 && !o->repair) {
		fprintf(stderr, "fsck.beamfs: pass 1: %u correctable error(s) found; rerun with --repair\n",
			total_corrected);
		return FSCK_UNCORRECTED;
	}

	if (o->repair && total_corrected > 0) {
		if (lseek(o->fd, 0, SEEK_SET) < 0 ||
		    write(o->fd, &sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
			fprintf(stderr, "fsck.beamfs: pass 1: write-back failed: %s\n",
				strerror(errno));
			return FSCK_ERROR;
		}
		if (o->verbose)
			printf("fsck.beamfs: pass 1: superblock repaired (%u correction(s))\n",
			       total_corrected);
		return FSCK_CORRECTED;
	}

	/*
	 * fsck says it and keeps going, where the kernel refuses.
	 *
	 * A checker that will not look at an old volume is no help to
	 * somebody trying to get data off one, and only the inode
	 * passes are affected -- they would report every inode as
	 * damaged against parity that is correct. Naming the reason
	 * once, and outside verbose, is better than a thousand decoder
	 * errors with no explanation.
	 */
	if (!(sb.s_feat_incompat & BEAMFS_FEATURE_INCOMPAT_INODE_CRC_FULL))
		fprintf(stderr,
			"fsck.beamfs: this volume predates INODE_CRC_FULL: i_crc32 covers the head of the inode and not the block pointers, so the inode passes below report damage that is not there\n");

	if (o->verbose)
		printf("fsck.beamfs: pass 1: superblock OK\n");
	return FSCK_OK;
}

/*
 * pass2_bitmap -- RS-decode every block bitmap block.
 *
 * Re-reads block 0 rather than sharing state with pass1_superblock:
 * passes are independent units, and by the time pass2 runs, run_passes()
 * has already confirmed opts->sb_trustworthy, so s_bitmap_blk and
 * s_bitmap_blocks_count are known-good.
 *
 * Each bitmap block interleaves 16 subblocks of 239 data + 16 parity
 * bytes (4080 bytes; the trailing 16 are unused pad, same layout as an
 * inline data block). This pass validates RS integrity only -- it does
 * not compare bitmap content against the inode table, which is pass 4's
 * job (bitmap rebuild). A subblock that RS cannot correct here means
 * fsck cannot trust which blocks that subblock's ~15296 bits claim as
 * free or allocated; pass 4 rebuilding the bitmap from the inode table
 * is the only way to recover from that, not attempted in this pass.
 */
static int pass2_bitmap(const struct fsck_opts *o)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs;
	uint8_t   block[BEAMFS_BLOCK_SIZE];
	ssize_t   got;
	uint32_t  bitmap_blocks_count;
	uint32_t  k;
	unsigned int total_corrected = 0;
	unsigned int uncorrectable_subblocks = 0;
	unsigned int uncorrectable_blocks = 0;
	int       positions[RS_NROOTS / 2];

	if (lseek(o->fd, 0, SEEK_SET) < 0 ||
	    read(o->fd, &sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
		fprintf(stderr, "fsck.beamfs: pass 2: cannot re-read block 0: %s\n",
			strerror(errno));
		return FSCK_ERROR;
	}

	bitmap_blocks_count = beamfs_bitmap_blocks_count_from_flags(sb.s_flags);
	if (bitmap_blocks_count == 0 || sb.s_bitmap_blk == 0) {
		fprintf(stderr, "fsck.beamfs: pass 2: invalid bitmap geometry (blk=%llu count=%u)\n",
			(unsigned long long)sb.s_bitmap_blk, bitmap_blocks_count);
		return FSCK_UNCORRECTED;
	}

	rs = rs_init();
	if (!rs) {
		fprintf(stderr, "fsck.beamfs: pass 2: rs_init failed\n");
		return FSCK_ERROR;
	}

	for (k = 0; k < bitmap_blocks_count; k++) {
		uint64_t disk_blk = sb.s_bitmap_blk + (uint64_t)k;
		unsigned int i;
		unsigned int block_corrected = 0;
		unsigned int block_uncorrectable = 0;

		if (lseek(o->fd, (off_t)disk_blk * BEAMFS_BLOCK_SIZE, SEEK_SET) < 0) {
			fprintf(stderr, "fsck.beamfs: pass 2: lseek bitmap blk %llu: %s\n",
				(unsigned long long)disk_blk, strerror(errno));
			rs_free(rs);
			return FSCK_ERROR;
		}
		got = read(o->fd, block, sizeof(block));
		if (got != (ssize_t)sizeof(block)) {
			fprintf(stderr, "fsck.beamfs: pass 2: short read on bitmap blk %llu (%zd/%zu)\n",
				(unsigned long long)disk_blk, got, sizeof(block));
			rs_free(rs);
			return FSCK_ERROR;
		}

		for (i = 0; i < BEAMFS_BITMAP_SUBBLOCKS; i++) {
			uint8_t *sub = block + i * BEAMFS_SUBBLOCK_TOTAL;
			int rc = rs_decode_subblock(rs, sub, BEAMFS_SUBBLOCK_DATA,
						    sub + BEAMFS_SUBBLOCK_DATA,
						    positions);
			if (rc == RS_UNCORRECTABLE) {
				block_uncorrectable++;
				if (o->verbose)
					fprintf(stderr, "fsck.beamfs: pass 2: bitmap blk %u sub %u uncorrectable\n",
						k, i);
			} else if (rc > 0) {
				block_corrected += (unsigned int)rc;
				if (o->verbose)
					fprintf(stderr, "fsck.beamfs: pass 2: bitmap blk %u sub %u: %d symbol(s) corrected\n",
						k, i, rc);
			}
		}

		if (block_uncorrectable > 0) {
			uncorrectable_blocks++;
			uncorrectable_subblocks += block_uncorrectable;
			fprintf(stderr, "fsck.beamfs: pass 2: bitmap blk %u: %u/%u subblocks uncorrectable\n",
				k, block_uncorrectable, BEAMFS_BITMAP_SUBBLOCKS);
			continue;
		}

		if (block_corrected > 0) {
			total_corrected += block_corrected;
			if (o->repair) {
				if (lseek(o->fd, (off_t)disk_blk * BEAMFS_BLOCK_SIZE, SEEK_SET) < 0 ||
				    write(o->fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
					fprintf(stderr, "fsck.beamfs: pass 2: write-back bitmap blk %u failed: %s\n",
						k, strerror(errno));
					rs_free(rs);
					return FSCK_ERROR;
				}
			}
		}
	}
	rs_free(rs);

	if (uncorrectable_blocks > 0) {
		fprintf(stderr, "fsck.beamfs: pass 2: %u bitmap block(s) RS-uncorrectable (%u subblock(s) total)\n",
			uncorrectable_blocks, uncorrectable_subblocks);
		return FSCK_UNCORRECTED;
	}

	if (total_corrected > 0) {
		if (!o->repair) {
			fprintf(stderr, "fsck.beamfs: pass 2: %u correctable error(s) found; rerun with --repair\n",
				total_corrected);
			return FSCK_UNCORRECTED;
		}
		if (o->verbose)
			printf("fsck.beamfs: pass 2: bitmap repaired (%u correction(s) across %u block(s))\n",
			       total_corrected, bitmap_blocks_count);
		return FSCK_CORRECTED;
	}

	if (o->verbose)
		printf("fsck.beamfs: pass 2: bitmap OK (%u block(s))\n", bitmap_blocks_count);
	return FSCK_OK;
}

/*
 * pass3_inode_walk -- CRC/RS validate every occupied inode, bounds-check
 * its direct and indirect pointers.
 *
 * Per-inode CRC32 + RS(255,239) over the 172-byte data region
 * (i_reserved[0..15] parity), matching kernel inode.c's Stage A/Stage B
 * order -- CRC first, RS only on mismatch, CRC re-verified after
 * correction. i_direct[] and the single/double/triple indirect trees
 * are bounds-checked via walk_indirect_tree(), mirroring the kernel's
 * beamfs_check_intermediate_block() (file_inline.c, 2026-08-24).
 *
 * No RS/CRC check on intermediate (indirect) blocks: the format has
 * none. This is a known residual, not an omission of this pass --
 * see data-protection-design.md section 6.1. A corrupted pointer
 * inside [data_start, data_start+nblocks) that happens to name another
 * allocated block is indistinguishable from a legitimate one, here as
 * in the kernel.
 */
/*
 * walk_indirect_tree -- bounds-check (and optionally mark-referenced)
 * every block reachable through an indirect/dindirect/tindirect
 * pointer tree.
 *
 * @level: 1 for i_indirect (block_no's 512 entries are leaf data
 *         blocks), 2 for i_dindirect (entries point to level-1
 *         blocks), 3 for i_tindirect (entries point to level-2 blocks).
 * @mark: optional callback invoked for every block visited (the
 *        intermediate block_no itself and every leaf it reaches),
 *        NULL for pass 3 (bounds-check only, no marking).
 *
 * No RS/CRC check on intermediate blocks: format-v6 has no per-block
 * checksum for them (data-protection-design.md section 6.1, the
 * residual scoped to scheme 2 and left to a future extent layout).
 * This can only catch a pointer landing outside
 * [data_start, data_start+nblocks) -- the same bound the kernel
 * enforces in file_inline.c -- not one landing on some other
 * in-range block.
 */
static void walk_indirect_tree(int fd, uint64_t block_no, int level,
			       uint64_t data_start, uint64_t nblocks,
			       void (*mark)(void *ctx, uint64_t phys),
			       void *ctx, unsigned int *bad_pointers)
{
	uint64_t ptrs[BEAMFS_INDIRECT_PTRS];
	unsigned int i;

	if (block_no < data_start || block_no >= data_start + nblocks) {
		(*bad_pointers)++;
		return;
	}
	if (mark)
		mark(ctx, block_no);

	if (lseek(fd, (off_t)block_no * BEAMFS_BLOCK_SIZE, SEEK_SET) < 0 ||
	    read(fd, ptrs, sizeof(ptrs)) != (ssize_t)sizeof(ptrs)) {
		(*bad_pointers)++;
		return;
	}

	for (i = 0; i < BEAMFS_INDIRECT_PTRS; i++) {
		uint64_t child = ptrs[i];

		if (child == 0)
			continue;
		if (level == 1) {
			if (child < data_start || child >= data_start + nblocks) {
				(*bad_pointers)++;
				continue;
			}
			if (mark)
				mark(ctx, child);
		} else {
			walk_indirect_tree(fd, child, level - 1, data_start,
					   nblocks, mark, ctx, bad_pointers);
		}
	}
}

static int pass3_inode_walk(const struct fsck_opts *o)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs;
	uint32_t inodes_per_block;
	uint64_t total_inodes;
	uint64_t data_start, nblocks;
	uint64_t ino;
	unsigned int total_corrected = 0;
	unsigned int uncorrectable_inodes = 0;
	unsigned int bad_pointer_inodes = 0;
	int       positions[RS_NROOTS / 2];

	if (lseek(o->fd, 0, SEEK_SET) < 0 ||
	    read(o->fd, &sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
		fprintf(stderr, "fsck.beamfs: pass 3: cannot re-read block 0: %s\n",
			strerror(errno));
		return FSCK_ERROR;
	}

	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	total_inodes = sb.s_inode_count;
	data_start   = sb.s_data_start_blk;
	nblocks      = sb.s_block_count - data_start;

	if (inodes_per_block == 0 || total_inodes == 0) {
		fprintf(stderr, "fsck.beamfs: pass 3: invalid inode geometry (per_block=%u count=%llu)\n",
			inodes_per_block, (unsigned long long)total_inodes);
		return FSCK_UNCORRECTED;
	}

	rs = rs_init();
	if (!rs) {
		fprintf(stderr, "fsck.beamfs: pass 3: rs_init failed\n");
		return FSCK_ERROR;
	}

	for (ino = 1; ino <= total_inodes; ino++) {
		uint64_t block  = sb.s_inode_table_blk + (ino - 1) / inodes_per_block;
		uint64_t offset = (ino - 1) % inodes_per_block;
		struct beamfs_inode raw;
		off_t   pos = (off_t)block * BEAMFS_BLOCK_SIZE
			    + (off_t)offset * sizeof(struct beamfs_inode);
		uint32_t crc;
		int      inode_dirty = 0;
		int      i;

		if (lseek(o->fd, pos, SEEK_SET) < 0 ||
		    read(o->fd, &raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
			fprintf(stderr, "fsck.beamfs: pass 3: cannot read inode %llu at block %llu: %s\n",
				(unsigned long long)ino, (unsigned long long)block,
				strerror(errno));
			rs_free(rs);
			return FSCK_ERROR;
		}

		if (raw.i_mode == 0)
			continue; /* free slot, not validated (matches ext2/e2fsck convention) */

		crc = crc32_inode(&raw);
		if (crc != raw.i_crc32) {
			int rc = rs_decode_subblock(rs, (uint8_t *)&raw, BEAMFS_INODE_RS_DATA,
						    raw.i_reserved, positions);
			if (rc == RS_UNCORRECTABLE) {
				uncorrectable_inodes++;
				fprintf(stderr, "fsck.beamfs: pass 3: inode %llu RS-uncorrectable\n",
					(unsigned long long)ino);
				continue;
			}
			crc = crc32_inode(&raw);
			if (crc != raw.i_crc32) {
				uncorrectable_inodes++;
				fprintf(stderr, "fsck.beamfs: pass 3: inode %llu CRC32 still wrong after RS correction\n",
					(unsigned long long)ino);
				continue;
			}
			if (o->verbose)
				fprintf(stderr, "fsck.beamfs: pass 3: inode %llu: %d symbol(s) corrected\n",
					(unsigned long long)ino, rc);
			total_corrected += (unsigned int)rc;
			inode_dirty = 1;
		}

		/*
		 * Fast symlinks keep their target in i_direct[] as raw bytes,
		 * up to 96 of them, not as block pointers. Reading those bytes
		 * as blocks yields whatever the path spelt -- fsstress names
		 * gave 0x7830783078307830, "0x0x0x0x" -- and 277 phantom
		 * out-of-range errors on a sound volume. super.c says the same
		 * where it refuses to free them: fast symlinks own no on-disk
		 * data blocks.
		 */
		if ((raw.i_mode & 0xF000) == 0xA000) {
			/*
			 * Two shapes, told apart by size. Under 96 bytes the
			 * target sits in i_direct[] as raw text and there is
			 * nothing to check: reading those bytes as blocks
			 * yields whatever the path spelt.
			 *
			 * At or above 96 the target lives in a data block
			 * and i_direct[0] names it. That one is a real
			 * pointer, and skipping it -- correct while the
			 * short form was the only form -- would report a
			 * legitimately owned block as unreferenced on every
			 * volume holding a long symlink.
			 */
			if (le64toh(raw.i_size) >= sizeof(raw.i_direct)) {
				uint64_t phys = le64toh(raw.i_direct[0]);

				if (phys < data_start ||
				    phys >= data_start + nblocks) {
					fprintf(stderr,
						"fsck.beamfs: pass 3: inode %llu symlink target block %llu out of [%llu, %llu)\n",
						(unsigned long long)ino,
						(unsigned long long)phys,
						(unsigned long long)data_start,
						(unsigned long long)(data_start + nblocks));
					bad_pointer_inodes++;
				}
			}
			continue;
		}

		for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
			uint64_t phys = raw.i_direct[i];

			if (phys == 0)
				continue;
			/*
			 * Inodes 1 and 2 are the root directory and the
			 * canary, and mkfs puts their blocks below
			 * data_start -- reserved, like the superblock and
			 * the bitmap, and outside what the allocator can
			 * ever hand out. The kernel accommodates this
			 * (beamfs_free_block silently drops anything below
			 * s_data_start); fsck did not, and reported two
			 * phantom errors on every freshly formatted volume.
			 *
			 * Only a pointer into the reserved area is excused,
			 * and only for those two inodes. Anything else below
			 * data_start is still a corrupted pointer.
			 */
			if (beamfs_ino_is_reserved(ino) && phys < data_start)
				continue;

			if (phys < data_start || phys >= data_start + nblocks) {
				bad_pointer_inodes++;
				fprintf(stderr, "fsck.beamfs: pass 3: inode %llu i_direct[%d]=%llu out of [%llu, %llu)\n",
					(unsigned long long)ino, i,
					(unsigned long long)phys,
					(unsigned long long)data_start,
					(unsigned long long)(data_start + nblocks));
			}
		}

		if (raw.i_indirect)
			walk_indirect_tree(o->fd, raw.i_indirect, 1, data_start,
					   nblocks, NULL, NULL, &bad_pointer_inodes);
		if (raw.i_dindirect)
			walk_indirect_tree(o->fd, raw.i_dindirect, 2, data_start,
					   nblocks, NULL, NULL, &bad_pointer_inodes);
		if (raw.i_tindirect)
			walk_indirect_tree(o->fd, raw.i_tindirect, 3, data_start,
					   nblocks, NULL, NULL, &bad_pointer_inodes);

		if (inode_dirty && o->repair) {
			if (lseek(o->fd, pos, SEEK_SET) < 0 ||
			    write(o->fd, &raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
				fprintf(stderr, "fsck.beamfs: pass 3: write-back inode %llu failed: %s\n",
					(unsigned long long)ino, strerror(errno));
				rs_free(rs);
				return FSCK_ERROR;
			}
		}
	}
	rs_free(rs);

	if (uncorrectable_inodes > 0 || bad_pointer_inodes > 0) {
		fprintf(stderr, "fsck.beamfs: pass 3: %u inode(s) RS-uncorrectable, %u out-of-range pointer(s)\n",
			uncorrectable_inodes, bad_pointer_inodes);
		return FSCK_UNCORRECTED;
	}

	if (total_corrected > 0) {
		if (!o->repair) {
			fprintf(stderr, "fsck.beamfs: pass 3: %u correctable error(s) found; rerun with --repair\n",
				total_corrected);
			return FSCK_UNCORRECTED;
		}
		if (o->verbose)
			printf("fsck.beamfs: pass 3: inode table repaired (%u correction(s))\n",
			       total_corrected);
		return FSCK_CORRECTED;
	}

	if (o->verbose)
		printf("fsck.beamfs: pass 3: inode table OK (%llu inode(s) walked)\n",
		       (unsigned long long)total_inodes);
	return FSCK_OK;
}

/*
 * pass4_bitmap_rebuild -- compare the on-disk bitmap against the set of
 * blocks actually referenced by the inode table (direct and indirect),
 * and (in --repair) rewrite the bitmap to match.
 *
 * Independent from pass3_inode_walk: each pass re-reads what it needs
 * rather than sharing state, matching the pattern already used by
 * passes 1-3. The double walk of the inode table (and, for files using
 * indirection, a second walk of the same pointer trees pass 3 already
 * walked) costs less than the added complexity of threading a shared
 * reference bitmap through two passes.
 *
 * Divergence classes reported, matching the kernel's bitmap semantics
 * (1 = free, 0 = used):
 *   - referenced-but-marked-free: an inode's direct pointer names a
 *     block the on-disk bitmap calls free. Silent data loss risk if
 *     that block is later handed out to another file.
 *   - marked-used-but-unreferenced: the bitmap calls a block used but
 *     no inode's direct pointers reference it. A leak, not a
 *     correctness risk, but reported since --repair would reclaim it.
 */
struct ref_ctx {
	uint8_t *reference;
	uint64_t data_start;
};

static void mark_reference(void *ctx_v, uint64_t phys)
{
	struct ref_ctx *ctx = ctx_v;
	uint64_t bit = phys - ctx->data_start;

	ctx->reference[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static int pass4_bitmap_rebuild(const struct fsck_opts *o)
{
	struct beamfs_super_block sb;
	uint8_t  *reference;   /* 1 bit per data block; 1 = referenced by some inode */
	uint32_t  inodes_per_block;
	uint64_t  total_inodes;
	uint64_t  data_start, nblocks;
	uint64_t  ino;
	uint64_t  reference_bytes;
	unsigned int referenced_but_free = 0;
	unsigned int used_but_unreferenced = 0;
	unsigned int excluded_inodes = 0;
	struct fsck_reader rd;
	enum fsck_read_status st;

	if (lseek(o->fd, 0, SEEK_SET) < 0 ||
	    read(o->fd, &sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
		fprintf(stderr, "fsck.beamfs: pass 4: cannot re-read block 0: %s\n",
			strerror(errno));
		return FSCK_ERROR;
	}

	inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	total_inodes = sb.s_inode_count;
	data_start   = sb.s_data_start_blk;
	nblocks      = sb.s_block_count - data_start;

	if (inodes_per_block == 0 || total_inodes == 0 || nblocks == 0) {
		fprintf(stderr, "fsck.beamfs: pass 4: invalid geometry, skipping\n");
		return FSCK_UNCORRECTED;
	}

	if (fsck_reader_open(&rd, o->fd) == FSCK_READ_UNCORRECTABLE) {
		fprintf(stderr, "fsck.beamfs: pass 4: superblock unreadable\n");
		return FSCK_ERROR;
	}
	reference_bytes = (nblocks + 7) / 8;
	reference = calloc(1, reference_bytes);
	if (!reference) {
		fprintf(stderr, "fsck.beamfs: pass 4: OOM allocating %llu-byte reference bitmap\n",
			(unsigned long long)reference_bytes);
		return FSCK_ERROR;
	}

	/* Build the reference set: every i_direct[] pointer of every
	 * occupied inode. No CRC/RS re-check here -- pass 3 already did
	 * that; a still-uncorrectable inode's pointers are simply skipped,
	 * consistent with pass 3 having already reported it.
	 */
	for (ino = 1; ino <= total_inodes; ino++) {
		struct beamfs_inode raw;
		int     i;

		/*
		 * Through the reader, so this pass sees the same inode pass 3 saw.
		 *
		 * It read the inode raw and pass 3 decoded it, so on a medium with
		 * a correctable error the two passes walked different trees -- and
		 * this is the pass that builds the reference set and reports
		 * used-but-unreferenced. One flipped bit in one inode was enough
		 * to have it mark blocks nobody uses and call the real ones lost.
		 */
		st = fsck_read_inode(&rd, ino, &raw);
		if (st == FSCK_READ_IO) {
			fprintf(stderr, "fsck.beamfs: pass 4: cannot read inode %llu\n",
				(unsigned long long)ino);
			fsck_reader_close(&rd);
			free(reference);
			return FSCK_ERROR;
		}
		if (st == FSCK_READ_UNCORRECTABLE) {
			/*
			 * Excluded rather than walked: its pointers are unknown,
			 * and following them turns one bad inode into a report of
			 * hundreds of lost blocks. pass 3 has already named it.
			 */
			excluded_inodes++;
			continue;
		}
		if (raw.i_mode == 0)
			continue;

		/*
		 * Fast symlinks keep their target in i_direct[] as raw bytes,
		 * up to 96 of them, not as block pointers. Reading those bytes
		 * as blocks yields whatever the path spelt -- fsstress names
		 * gave 0x7830783078307830, "0x0x0x0x" -- and 277 phantom
		 * out-of-range errors on a sound volume. super.c says the same
		 * where it refuses to free them: fast symlinks own no on-disk
		 * data blocks.
		 */
		if ((raw.i_mode & 0xF000) == 0xA000) {
			/*
			 * Two shapes, told apart by size. Under 96 bytes the
			 * target sits in i_direct[] as raw text and there is
			 * nothing to check: reading those bytes as blocks
			 * yields whatever the path spelt.
			 *
			 * At or above 96 the target lives in a data block
			 * and i_direct[0] names it. That one is a real
			 * pointer, and skipping it -- correct while the
			 * short form was the only form -- would report a
			 * legitimately owned block as unreferenced on every
			 * volume holding a long symlink.
			 */
			if (le64toh(raw.i_size) >= sizeof(raw.i_direct)) {
				uint64_t phys = le64toh(raw.i_direct[0]);

				if (phys >= data_start &&
				    phys < data_start + nblocks) {
					uint64_t bit = phys - data_start;

					reference[bit / 8] |=
						(uint8_t)(1u << (bit % 8));
				}
			}
			continue;
		}

		for (i = 0; i < BEAMFS_DIRECT_BLOCKS; i++) {
			uint64_t phys = raw.i_direct[i];
			uint64_t bit;

			if (phys == 0)
				continue;
			if (phys < data_start || phys >= data_start + nblocks)
				continue; /* out-of-range, already reported by pass 3 */
			bit = phys - data_start;
			reference[bit / 8] |= (uint8_t)(1u << (bit % 8));
		}

		{
			struct ref_ctx rctx = { reference, data_start };
			unsigned int dummy_bad = 0; /* pass 3 already reports these */

			if (raw.i_indirect)
				walk_indirect_tree(o->fd, raw.i_indirect, 1, data_start,
						   nblocks, mark_reference, &rctx, &dummy_bad);
			if (raw.i_dindirect)
				walk_indirect_tree(o->fd, raw.i_dindirect, 2, data_start,
						   nblocks, mark_reference, &rctx, &dummy_bad);
			if (raw.i_tindirect)
				walk_indirect_tree(o->fd, raw.i_tindirect, 3, data_start,
						   nblocks, mark_reference, &rctx, &dummy_bad);
		}
	}

	/* Compare against the on-disk bitmap, one bitmap block (16
	 * interleaved subblocks) at a time. This intentionally re-derives
	 * bits from the raw RS-decoded subblock data rather than reusing
	 * pass 2's result, for the same independence reason as above; a
	 * subblock pass 2 already flagged uncorrectable is skipped here
	 * (its bits cannot be trusted either way).
	 */
	{
		struct rs_codec *rs = rs_init();
		uint32_t bitmap_blocks_count;
		uint32_t k;
		uint64_t bit_global = 0;
		int positions[RS_NROOTS / 2];

		if (!rs) {
			fprintf(stderr, "fsck.beamfs: pass 4: rs_init failed\n");
			free(reference);
			return FSCK_ERROR;
		}
		bitmap_blocks_count = beamfs_bitmap_blocks_count_from_flags(sb.s_flags);

		for (k = 0; k < bitmap_blocks_count && bit_global < nblocks; k++) {
			uint64_t disk_blk = sb.s_bitmap_blk + (uint64_t)k;
			uint8_t  block[BEAMFS_BLOCK_SIZE];
			unsigned int i;
			int block_dirty = 0;

			if (lseek(o->fd, (off_t)disk_blk * BEAMFS_BLOCK_SIZE, SEEK_SET) < 0 ||
			    read(o->fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
				fprintf(stderr, "fsck.beamfs: pass 4: cannot read bitmap blk %llu: %s\n",
					(unsigned long long)disk_blk, strerror(errno));
				rs_free(rs);
				free(reference);
				return FSCK_ERROR;
			}

			for (i = 0; i < BEAMFS_BITMAP_SUBBLOCKS && bit_global < nblocks; i++) {
				uint8_t *sub = block + i * BEAMFS_SUBBLOCK_TOTAL;
				int rc = rs_decode_subblock(rs, sub, BEAMFS_SUBBLOCK_DATA,
							    sub + BEAMFS_SUBBLOCK_DATA,
							    positions);
				unsigned long b;

				if (rc == RS_UNCORRECTABLE) {
					bit_global += BEAMFS_SUBBLOCK_DATA * 8;
					continue; /* untrustworthy, already reported by pass 2 */
				}

				for (b = 0; b < (unsigned long)BEAMFS_SUBBLOCK_DATA * 8 &&
					    bit_global < nblocks; b++, bit_global++) {
					int disk_free = (sub[b / 8] & (1u << (b % 8))) != 0;
					int ref_used  = (reference[bit_global / 8] &
							 (1u << (bit_global % 8))) != 0;

					if (ref_used && disk_free) {
						referenced_but_free++;
						if (o->verbose)
							fprintf(stderr, "fsck.beamfs: pass 4: block %llu referenced but bitmap marks free\n",
								(unsigned long long)(data_start + bit_global));
						if (o->repair) {
							sub[b / 8] &= (uint8_t)~(1u << (b % 8));
							block_dirty = 1;
						}
					} else if (!ref_used && !disk_free) {
						used_but_unreferenced++;
						if (o->verbose)
							fprintf(stderr, "fsck.beamfs: pass 4: block %llu marked used but unreferenced\n",
								(unsigned long long)(data_start + bit_global));
						if (o->repair) {
							sub[b / 8] |= (uint8_t)(1u << (b % 8));
							block_dirty = 1;
						}
					}
				}
			}

			if (block_dirty) {
				if (lseek(o->fd, (off_t)disk_blk * BEAMFS_BLOCK_SIZE, SEEK_SET) < 0 ||
				    write(o->fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
					fprintf(stderr, "fsck.beamfs: pass 4: write-back bitmap blk %llu failed: %s\n",
						(unsigned long long)disk_blk, strerror(errno));
					rs_free(rs);
					free(reference);
					return FSCK_ERROR;
				}
			}
		}
		rs_free(rs);
	}
	free(reference);

	if (excluded_inodes || rd.corrected)
		fprintf(stderr, "fsck.beamfs: pass 4: %u read(s) RS-corrected, %u inode(s) excluded\n",
			rd.corrected, excluded_inodes);
	fsck_reader_close(&rd);
	if (referenced_but_free == 0 && used_but_unreferenced == 0) {
		if (o->verbose)
			printf("fsck.beamfs: pass 4: bitmap consistent with inode table\n");
		return FSCK_OK;
	}

	fprintf(stderr, "fsck.beamfs: pass 4: %u referenced-but-free, %u used-but-unreferenced block(s)\n",
		referenced_but_free, used_but_unreferenced);

	if (!o->repair) {
		fprintf(stderr, "fsck.beamfs: pass 4: rerun with --repair to regenerate the bitmap\n");
		return FSCK_UNCORRECTED;
	}

	if (o->verbose)
		printf("fsck.beamfs: pass 4: bitmap regenerated from inode table\n");
	return FSCK_CORRECTED;
}

/*
 * pass5_rs_journal -- validate the 64-entry RS event journal.
 *
 * The journal lives inside s_rs_journal[], part of the superblock's
 * RS-covered region B (see pass1_superblock and mkfs.beamfs's
 * sb_to_rs_staging). By the time pass 5 runs, run_passes() has already
 * confirmed opts->sb_trustworthy, so the journal bytes read here have
 * already been through RS correction if any was needed -- there is no
 * separate RS layer for the journal itself to decode. This pass checks
 * logical consistency instead:
 *   - per-entry CRC32 over the first 32 bytes (re_crc32 covers
 *     everything up to itself, kernel super.c::beamfs_log_rs_event_flagged);
 *   - re_reserved and re_pad are structural sentinels that the kernel
 *     always writes as zero -- any other value means either a bug or a
 *     corruption CRC/RS did not catch, and is reported;
 *   - timestamps non-decreasing when the journal is read in write order
 *     starting at s_rs_journal_head (the oldest surviving entry after
 *     circular wraparound). A slot is only considered "populated" if
 *     its own CRC32 passes and re_timestamp != 0, since an unwritten
 *     slot on a freshly formatted volume is legitimately zero and
 *     should not be flagged as a timestamp regression.
 *
 * There is nothing to repair here in the RS-correction sense: the
 * journal is a forensic log, not redo state (fsck.beamfs.md section 2
 * explicitly rules out journal replay). --repair zeroes any entry that
 * fails its own CRC32, which is the same "restate from what is
 * independently known" doctrine used for s_crc32 in pass 1 -- a slot
 * whose own checksum disagrees with its content cannot be trusted, and
 * dropping it (zero timestamp, zero flags) is safer than guessing.
 */
static int pass5_rs_journal(const struct fsck_opts *o)
{
	struct beamfs_super_block sb;
	unsigned int bad_crc = 0;
	unsigned int bad_sentinel = 0;
	unsigned int reordered = 0;
	uint64_t     prev_ts = 0;
	int          seen_first = 0;
	uint8_t      head;
	unsigned int n;
	int          dirty = 0;

	if (lseek(o->fd, 0, SEEK_SET) < 0 ||
	    read(o->fd, &sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
		fprintf(stderr, "fsck.beamfs: pass 5: cannot re-read block 0: %s\n",
			strerror(errno));
		return FSCK_ERROR;
	}

	head = (uint8_t)(sb.s_rs_journal_head % BEAMFS_RS_JOURNAL_SIZE);

	for (n = 0; n < BEAMFS_RS_JOURNAL_SIZE; n++) {
		unsigned int idx = (head + n) % BEAMFS_RS_JOURNAL_SIZE;
		struct beamfs_rs_event *ev = &sb.s_rs_journal[idx];
		uint32_t crc;

		if (ev->re_timestamp == 0 && ev->re_crc32 == 0 &&
		    ev->re_block_no == 0)
			continue; /* unwritten slot, legitimate on a fresh volume */

		crc = crc32(ev, offsetof(struct beamfs_rs_event, re_crc32));
		if (crc != ev->re_crc32) {
			bad_crc++;
			fprintf(stderr, "fsck.beamfs: pass 5: journal slot %u CRC32 mismatch (have 0x%08x, want 0x%08x)\n",
				idx, ev->re_crc32, crc);
			if (o->repair) {
				memset(ev, 0, sizeof(*ev));
				dirty = 1;
			}
			continue; /* don't trust timestamp/sentinels of a bad-CRC entry */
		}

		if (ev->re_reserved != 0 || ev->re_pad != 0) {
			bad_sentinel++;
			fprintf(stderr, "fsck.beamfs: pass 5: journal slot %u sentinel nonzero (reserved=0x%08x pad=0x%08x)\n",
				idx, ev->re_reserved, ev->re_pad);
		}

		if (seen_first && ev->re_timestamp < prev_ts) {
			reordered++;
			if (o->verbose)
				fprintf(stderr, "fsck.beamfs: pass 5: journal slot %u timestamp %llu precedes previous %llu\n",
					idx, (unsigned long long)ev->re_timestamp,
					(unsigned long long)prev_ts);
		}
		prev_ts = ev->re_timestamp;
		seen_first = 1;
	}

	if (dirty && o->repair) {
		/* s_rs_journal[] lives inside the superblock's RS-covered
		 * region B (pass1_superblock, mkfs.beamfs::sb_to_rs_staging).
		 * Zeroing a bad-CRC entry in memory without re-deriving
		 * s_crc32 and the SB's own RS parity leaves the superblock
		 * internally inconsistent: the next pass1 run would find the
		 * modified bytes disagreeing with parity computed over the
		 * old bytes and report the whole superblock RS-uncorrectable.
		 * Caught by testing this pass against a synthetic volume
		 * (2026-08-25): repair reported success, but the immediate
		 * re-check failed at pass 1.
		 */
		struct rs_codec *rs = rs_init();
		uint8_t  staging[BEAMFS_SB_RS_STAGING_BYTES];
		uint8_t *sb_parity = (uint8_t *)&sb + BEAMFS_SB_RS_PARITY_OFFSET;
		const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
		const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
		const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);
		unsigned int si;

		if (!rs) {
			fprintf(stderr, "fsck.beamfs: pass 5: rs_init failed\n");
			return FSCK_ERROR;
		}

		sb.s_crc32 = crc32_sb(&sb);

		memset(staging, 0, sizeof(staging));
		memcpy(staging, &sb, off_crc32);
		memcpy(staging + off_crc32, (uint8_t *)&sb + off_uuid, off_pad - off_uuid);
		for (si = 0; si < BEAMFS_SB_RS_SUBBLOCKS; si++)
			rs_encode_subblock(rs, staging + si * BEAMFS_SB_RS_DATA_LEN,
					   BEAMFS_SB_RS_DATA_LEN,
					   sb_parity + si * RS_NROOTS);
		rs_free(rs);

		if (lseek(o->fd, 0, SEEK_SET) < 0 ||
		    write(o->fd, &sb, sizeof(sb)) != (ssize_t)sizeof(sb)) {
			fprintf(stderr, "fsck.beamfs: pass 5: write-back failed: %s\n",
				strerror(errno));
			return FSCK_ERROR;
		}
	}

	if (bad_sentinel > 0 || reordered > 0) {
		/* Sentinel violations and reordering are forensic anomalies,
		 * not correctable by this pass (there is no independent
		 * source of truth for what the journal should say). Report
		 * only.
		 */
		fprintf(stderr, "fsck.beamfs: pass 5: %u sentinel violation(s), %u timestamp reordering(s) (report-only)\n",
			bad_sentinel, reordered);
	}

	if (bad_crc > 0) {
		if (!o->repair) {
			fprintf(stderr, "fsck.beamfs: pass 5: %u journal entrie(s) with bad CRC32; rerun with --repair to drop them\n",
				bad_crc);
			return FSCK_UNCORRECTED;
		}
		if (o->verbose)
			printf("fsck.beamfs: pass 5: %u journal entrie(s) dropped (bad CRC32)\n", bad_crc);
		return FSCK_CORRECTED;
	}

	if (o->verbose)
		printf("fsck.beamfs: pass 5: RS journal OK\n");
	return FSCK_OK;
}

/*
 * run_passes -- drive all 5 passes, accumulating the worst outcome.
 *
 * FSCK_ERROR and FSCK_USAGE are the only fatal codes: they abort the
 * remaining passes immediately (operational failure -- can't open the
 * device, bad argument, etc.). FSCK_CORRECTED and FSCK_UNCORRECTED are
 * findings, not failures: stopping at the first one would mean a
 * single repaired flip in pass 1 silences passes 2-5 entirely, which
 * defeats the purpose of a multi-pass checker. Each pass runs and the
 * highest-severity code seen (FSCK_UNCORRECTED > FSCK_CORRECTED >
 * FSCK_OK) is returned at the end.
 *
 * pass1_superblock() sets opts->sb_trustworthy; if it comes back false
 * (bad magic without --force, or RS-uncorrectable superblock), passes
 * 2-5 are skipped rather than run against block offsets derived from
 * fields that failed validation.
 */
static int run_passes(struct fsck_opts *o)
{
	int rc, worst = FSCK_OK;

	rc = pass1_superblock(o);
	if (rc == FSCK_ERROR || rc == FSCK_USAGE)
		return rc;
	if (rc > worst)
		worst = rc;

	if (!o->sb_trustworthy) {
		fprintf(stderr, "fsck.beamfs: superblock not trustworthy, skipping passes 2-5\n");
		return worst > FSCK_UNCORRECTED ? worst : FSCK_UNCORRECTED;
	}

	rc = pass2_bitmap(o);
	if (rc == FSCK_ERROR || rc == FSCK_USAGE)
		return rc;
	if (rc > worst)
		worst = rc;

	rc = pass3_inode_walk(o);
	if (rc == FSCK_ERROR || rc == FSCK_USAGE)
		return rc;
	if (rc > worst)
		worst = rc;

	rc = pass4_bitmap_rebuild(o);
	if (rc == FSCK_ERROR || rc == FSCK_USAGE)
		return rc;
	if (rc > worst)
		worst = rc;

	rc = pass5_rs_journal(o);

	/*
	 * pass 6, after the block-level passes: it needs a superblock and a
	 * bitmap it can trust, and it reports on the filesystem rather than
	 * on its blocks.
	 */
	{
		struct fsck_reader rd6;
		struct fsck_pass6_opts p6o = { .verbose = o->verbose, .repair = o->repair };
		struct fsck_pass6_result p6r;

		if (fsck_reader_open(&rd6, o->fd) == FSCK_READ_UNCORRECTABLE) {
			fprintf(stderr, "fsck.beamfs: pass 6: superblock unreadable\n");
			worst = FSCK_UNCORRECTED;
		} else {
			int p6 = fsck_pass6(&rd6, &p6o, &p6r);

			fsck_pass6_report(&p6r);
			fsck_reader_close(&rd6);
			if (p6 == FSCK_PASS6_ERROR)
				worst = FSCK_ERROR;
			else if (p6 == FSCK_PASS6_UNCORRECTED && worst < FSCK_UNCORRECTED)
				worst = FSCK_UNCORRECTED;
		}
	}
	if (rc == FSCK_ERROR || rc == FSCK_USAGE)
		return rc;
	if (rc > worst)
		worst = rc;

	return worst;
}

int main(int argc, char **argv)
{
	struct fsck_opts opts = {
		.device         = NULL,
		.check_only     = true,
		.repair         = false,
		.force          = false,
		.verbose        = false,
		.sb_trustworthy = true,
	};
	int fd;

	static const struct option long_opts[] = {
		{ "check-only", no_argument, NULL, 'n' },
		{ "repair",     no_argument, NULL, 'p' },
		{ "yes",        no_argument, NULL, 'y' },
		{ "force",      no_argument, NULL, 'f' },
		{ "verbose",    no_argument, NULL, 'v' },
		{ "version",    no_argument, NULL, 'V' },
		{ "help",       no_argument, NULL, 'h' },
		{ NULL,         0,           NULL,  0  },
	};

	int c;

	while ((c = getopt_long(argc, argv, "npyfvVh", long_opts, NULL)) != -1) {
		switch (c) {
		case 'n':
			opts.check_only = true;
			opts.repair     = false;
			break;
		case 'p':
		case 'y':
			/*
			 * -y is what every other fsck calls this, and what
			 * callers pass without checking whether a
			 * particular one understands it: e2fsck, fsck.vfat
			 * and fsck.xfs all take it to mean repair without
			 * asking. Refusing it returned exit code 16, usage
			 * error, and xfstests read that as the check
			 * failing -- generic/441 reports "fsck.beamfs
			 * failed, err=16" on a filesystem that was
			 * perfectly sound.
			 *
			 * There is nothing to prompt about here: this fsck
			 * never asks a question, so -y and --repair are the
			 * same request.
			 */
			opts.repair     = true;
			opts.check_only = false;
			break;
		case 'f':
			opts.force = true;
			break;
		case 'v':
			opts.verbose = true;
			break;
		case 'V':
			version();
			return FSCK_OK;
		case 'h':
			print_usage(stdout, argv[0]);
			return FSCK_OK;
		default:
			print_usage(stderr, argv[0]);
			return FSCK_USAGE;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "fsck.beamfs: missing device argument\n");
		print_usage(stderr, argv[0]);
		return FSCK_USAGE;
	}
	opts.device = argv[optind];

	fd = open(opts.device, opts.repair ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "fsck.beamfs: cannot open %s: %s\n",
			opts.device, strerror(errno));
		return FSCK_ERROR;
	}
	opts.fd = fd;

	if (opts.verbose)
		printf("fsck.beamfs %s: checking %s (%s mode)\n",
		       FSCK_BEAMFS_VERSION, opts.device,
		       opts.repair ? "repair" : "check-only");

	{
		int rc = run_passes(&opts);

		close(fd);
		return rc;
	}
}
