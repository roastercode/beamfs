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
static int pass1_superblock(const struct fsck_opts *o)
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

	if (o->verbose)
		printf("fsck.beamfs: pass 1: superblock OK\n");
	return FSCK_OK;
}

static int pass2_bitmap(const struct fsck_opts *o)
{
	(void)o;
	fprintf(stderr, "fsck.beamfs: pass 2 (bitmap) not yet implemented\n");
	return FSCK_OK;
}

static int pass3_inode_walk(const struct fsck_opts *o)
{
	(void)o;
	fprintf(stderr, "fsck.beamfs: pass 3 (inode walk) not yet implemented\n");
	return FSCK_OK;
}

static int pass4_bitmap_rebuild(const struct fsck_opts *o)
{
	(void)o;
	fprintf(stderr, "fsck.beamfs: pass 4 (bitmap rebuild) not yet implemented\n");
	return FSCK_OK;
}

static int pass5_rs_journal(const struct fsck_opts *o)
{
	(void)o;
	fprintf(stderr, "fsck.beamfs: pass 5 (RS journal) not yet implemented\n");
	return FSCK_OK;
}

static int run_passes(const struct fsck_opts *o)
{
	int rc;

	rc = pass1_superblock(o);
	if (rc != FSCK_OK)
		return rc;
	rc = pass2_bitmap(o);
	if (rc != FSCK_OK)
		return rc;
	rc = pass3_inode_walk(o);
	if (rc != FSCK_OK)
		return rc;
	rc = pass4_bitmap_rebuild(o);
	if (rc != FSCK_OK)
		return rc;
	rc = pass5_rs_journal(o);
	if (rc != FSCK_OK)
		return rc;

	return FSCK_OK;
}

int main(int argc, char **argv)
{
	struct fsck_opts opts = {
		.device     = NULL,
		.check_only = true,
		.repair     = false,
		.force      = false,
		.verbose    = false,
	};
	int fd;

	static const struct option long_opts[] = {
		{ "check-only", no_argument, NULL, 'n' },
		{ "repair",     no_argument, NULL, 'p' },
		{ "force",      no_argument, NULL, 'f' },
		{ "verbose",    no_argument, NULL, 'v' },
		{ "version",    no_argument, NULL, 'V' },
		{ "help",       no_argument, NULL, 'h' },
		{ NULL,         0,           NULL,  0  },
	};

	int c;

	while ((c = getopt_long(argc, argv, "npfvVh", long_opts, NULL)) != -1) {
		switch (c) {
		case 'n':
			opts.check_only = true;
			opts.repair     = false;
			break;
		case 'p':
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
