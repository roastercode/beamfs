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
static int pass1_superblock(const struct fsck_opts *o)
{
	(void)o;
	fprintf(stderr, "fsck.beamfs: pass 1 (superblock) not yet implemented\n");
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
	close(fd);

	if (opts.verbose)
		printf("fsck.beamfs %s: checking %s (%s mode)\n",
		       FSCK_BEAMFS_VERSION, opts.device,
		       opts.repair ? "repair" : "check-only");

	return run_passes(&opts);
}
