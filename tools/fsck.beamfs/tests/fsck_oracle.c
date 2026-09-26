// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck_oracle -- does the checker report what is actually wrong?
 *
 * Every number this project has acted on for two days came from
 * fsck.beamfs, and nothing has ever checked fsck.beamfs. Two defects
 * found by reading it -- pass 4 walking inodes it had not corrected,
 * the indirect walk following pointers it had not verified -- would
 * both have produced leaks that were not there. Whether they did is
 * unknown, because there was no way to ask.
 *
 * This asks. It builds a volume whose exact state is known, damages it
 * in one known way, and requires the checker to report that one thing
 * and nothing else. A checker that says "0 lost" on a volume with one
 * leaked block is useless; so is one that says "3 lost" on a volume
 * with one.
 *
 * Each case re-encodes the Reed-Solomon parity over whatever it
 * changed. Without that the checker corrects the injected damage on
 * read and the case tests nothing -- which is the trap a first attempt
 * fell into: a flipped bitmap bit that RS quietly put back.
 *
 * Usage: fsck_oracle <fsck-binary> <mkfs-binary> [image]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "beamfs_format.h"
#include "crc32.h"
#include "rs_decode.h"

#define IMAGE_BYTES (256ull * 1024 * 1024)
#define INODES      4096

static const char *fsck_bin;
static const char *mkfs_bin;
static const char *image;
static unsigned int cases_run, cases_failed;
static const char *only;

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

static int run(const char *fmt, ...)
{
	char cmd[1024];
	va_list ap;
	int rc;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	rc = system(cmd);
	return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* A fresh volume, every case starting from the same known state. */
static void fresh(void)
{
	if (run("rm -f %s && truncate -s %llu %s", image,
		(unsigned long long)IMAGE_BYTES, image) != 0)
		die("cannot create %s", image);
	if (run("%s -N %d %s >/dev/null 2>&1", mkfs_bin, INODES, image) != 0)
		die("mkfs failed on %s", image);
}

/*
 * What the checker reported.
 *
 * Parsed from its own output rather than from an exit code: the exit
 * code says something is wrong, and the whole question here is whether
 * it is the right something and how much of it.
 */
struct verdict {
	int  rc;
	long lost;        /* used-but-unreferenced */
	long dangling;    /* referenced-but-free */
	bool sb_bad;
	bool pass4_ran;
	bool uncorrectable;
	long free_dirents;  /* pass 6: entries naming a free inode */
	long orphans;       /* pass 6: allocated inodes nothing reaches */
};

static struct verdict check(void)
{
	struct verdict v = { .rc = 0, .lost = 0, .dangling = 0 };
	char line[512];
	FILE *f;
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "%s -v %s 2>&1", fsck_bin, image);
	f = popen(cmd, "r");
	if (!f)
		die("cannot run %s", fsck_bin);

	while (fgets(line, sizeof(line), f)) {
		long a, b;

		if (sscanf(line, "fsck.beamfs: pass 4: %ld referenced-but-free, %ld used-but-unreferenced",
			   &a, &b) == 2) {
			v.dangling = a;
			v.lost = b;
			v.pass4_ran = true;
		}
		/*
		 * The two counts generic/076 came back with. Parsed like
		 * the others: what matters is not that the checker says
		 * something is wrong but that it says the right thing and
		 * the right number of times.
		 */
		/*
		 * strstr first, sscanf second.
		 *
		 * sscanf returns how many conversions succeeded, not whether
		 * the literal text matched: "%ld directory entr" against
		 * "1 inode(s) whose size runs ahead" converts the 1, returns
		 * 1, and the line is taken for something it is not. Both
		 * counters read each other's lines that way, and two cases
		 * failed against a checker that was right.
		 */
		if (strstr(line, "pass 6:") &&
		    strstr(line, "directory entr") &&
		    strstr(line, "naming a free inode") &&
		    sscanf(line, "fsck.beamfs: pass 6: %ld", &a) == 1)
			v.free_dirents = a;
		if (strstr(line, "pass 6:") &&
		    strstr(line, "allocated inode(s) no directory reaches") &&
		    sscanf(line, "fsck.beamfs: pass 6: %ld", &a) == 1)
			v.orphans = a;
		if (strstr(line, "pass 4: bitmap consistent"))
			v.pass4_ran = true;
		if (strstr(line, "superblock unreadable") ||
		    strstr(line, "pass 1: superblock RS-uncorrectable"))
			v.sb_bad = true;
		if (strstr(line, "uncorrectable"))
			v.uncorrectable = true;
	}
	v.rc = pclose(f);
	v.rc = WIFEXITED(v.rc) ? WEXITSTATUS(v.rc) : -1;
	return v;
}

static void report(const char *name, bool ok, const char *fmt, ...)
{
	va_list ap;

	cases_run++;
	if (!ok)
		cases_failed++;
	printf("  %-38s %s", name, ok ? "ok" : "FAILED");
	if (fmt && *fmt) {
		fputs("  -- ", stdout);
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
	putchar('\n');
}

/* ---- reading and writing the image, with parity kept correct ---- */

static void pread_at(int fd, off_t off, void *buf, size_t len)
{
	if (pread(fd, buf, len, off) != (ssize_t)len)
		die("short read at %lld", (long long)off);
}

static void pwrite_at(int fd, off_t off, const void *buf, size_t len)
{
	if (pwrite(fd, buf, len, off) != (ssize_t)len)
		die("short write at %lld", (long long)off);
}

static void read_sb(int fd, struct beamfs_super_block *sb)
{
	pread_at(fd, 0, sb, sizeof(*sb));
}

/*
 * Mark a data block used or free in the on-disk bitmap, parity and all.
 *
 * 1 is free and 0 is used, following the kernel: clear_bit allocates.
 */
static bool set_block_used(int fd, const struct beamfs_super_block *sb,
			   uint64_t phys, bool used, struct rs_codec *rs)
{
	uint64_t bit;

	/*
	 * Not every block a file points at is inside the bitmap.
	 *
	 * mkfs puts the canary at data_start - 1, outside the data
	 * region and therefore outside the allocation bitmap: it is
	 * never marked used and pass 4 can neither account for it nor
	 * report it. A first version of this subtracted data_start
	 * anyway and read at an offset of 1.9 TB into a 256 MB image.
	 */
	if (phys < sb->s_data_start_blk ||
	    phys >= sb->s_block_count)
		return false;

	bit = phys - sb->s_data_start_blk;
	uint32_t which = (uint32_t)(bit / BEAMFS_BITS_PER_BITMAP_BLOCK);
	uint64_t within = bit % BEAMFS_BITS_PER_BITMAP_BLOCK;
	off_t blk_off = (off_t)(sb->s_bitmap_blk + which) * BEAMFS_BLOCK_SIZE;
	uint8_t blk[BEAMFS_BLOCK_SIZE];
	uint8_t flat[BEAMFS_BITMAP_DATA_BYTES];
	unsigned int i;

	pread_at(fd, blk_off, blk, sizeof(blk));

	/* Decode into a flat view: the bitmap is stored as sixteen
	 * subblocks of data followed by their parity, and a bit index
	 * means nothing until the parity is taken out of the way.
	 */
	for (i = 0; i < BEAMFS_BITMAP_SUBBLOCKS; i++)
		memcpy(flat + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       blk + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       BEAMFS_SUBBLOCK_DATA);

	if (used)
		flat[within / 8] &= (uint8_t)~(1u << (within % 8));
	else
		flat[within / 8] |= (uint8_t)(1u << (within % 8));

	/* Back into the on-disk layout, re-encoding parity over what
	 * changed. Skipping this is how an injected defect gets
	 * corrected away before the checker ever sees it.
	 */
	for (i = 0; i < BEAMFS_BITMAP_SUBBLOCKS; i++) {
		uint8_t *sub = blk + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;

		memcpy(sub, flat + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
		rs_encode_subblock(rs, sub, BEAMFS_SUBBLOCK_DATA,
				   sub + BEAMFS_SUBBLOCK_DATA);
	}
	pwrite_at(fd, blk_off, blk, sizeof(blk));
	return true;
}

/*
 * Write an inode back with its CRC and RS parity made correct.
 *
 * An inode written without this is damaged rather than changed, and the
 * checker reports it as damaged -- which tests the RS path, not the
 * case at hand.
 */
static void write_inode(int fd, const struct beamfs_super_block *sb,
			uint64_t ino, struct beamfs_inode *in,
			struct rs_codec *rs)
{
	uint32_t per = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	uint64_t blk = sb->s_inode_table_blk + (ino - 1) / per;
	uint64_t idx = (ino - 1) % per;
	off_t off = (off_t)blk * BEAMFS_BLOCK_SIZE
		  + (off_t)idx * sizeof(struct beamfs_inode);

	in->i_crc32 = crc32_inode(in);
	rs_encode_subblock(rs, (uint8_t *)in, BEAMFS_INODE_RS_DATA,
			   in->i_reserved);
	pwrite_at(fd, off, in, sizeof(*in));
}

static void read_inode(int fd, const struct beamfs_super_block *sb,
		       uint64_t ino, struct beamfs_inode *out)
{
	uint32_t per = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	uint64_t blk = sb->s_inode_table_blk + (ino - 1) / per;
	uint64_t idx = (ino - 1) % per;

	pread_at(fd, (off_t)blk * BEAMFS_BLOCK_SIZE
		 + (off_t)idx * sizeof(struct beamfs_inode), out, sizeof(*out));
}

/*
 * Give a fabricated inode a name in the root.
 *
 * pass 6 reports an allocated inode no directory reaches, and it is
 * right to: a case that leaves one behind is testing its own sloppiness
 * rather than the thing it meant to test. The record goes in the free
 * space at the end of the root's block, which mkfs leaves as one large
 * empty record.
 */
static void link_into_root(int fd, const struct beamfs_super_block *sb,
			   uint64_t ino, const char *name, struct rs_codec *rs)
{
	struct beamfs_inode root;
	uint8_t blk[BEAMFS_BLOCK_SIZE];
	uint8_t flat[BEAMFS_DATA_INLINE_BYTES];
	uint32_t off = 0, need;
	unsigned int i;
	size_t nl = strlen(name);
	off_t blk_off;

	read_inode(fd, sb, 1, &root);
	if (!root.i_direct[0])
		return;
	blk_off = (off_t)root.i_direct[0] * BEAMFS_BLOCK_SIZE;
	pread_at(fd, blk_off, blk, sizeof(blk));
	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
		memcpy(flat + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       blk + (size_t)i * BEAMFS_SUBBLOCK_TOTAL,
		       BEAMFS_SUBBLOCK_DATA);

	need = (uint32_t)((BEAMFS_DIRENT_HDR_LEN + nl + BEAMFS_DIRENT_ALIGN - 1)
			  & ~(size_t)(BEAMFS_DIRENT_ALIGN - 1));

	/* Walk to the trailing free record and split it. */
	while (off + BEAMFS_DIRENT_HDR_LEN <= BEAMFS_DATA_INLINE_BYTES) {
		struct beamfs_dir_entry *de = (struct beamfs_dir_entry *)(flat + off);
		uint16_t rec = le16toh(de->d_rec_len);

		if (rec == 0)
			return;
		if (le64toh(de->d_ino) == 0 && rec >= need + BEAMFS_DIRENT_MIN_LEN) {
			struct beamfs_dir_entry *nd =
				(struct beamfs_dir_entry *)(flat + off + need);

			nd->d_ino = 0;
			nd->d_rec_len = htole16((uint16_t)(rec - need));
			nd->d_name_len = 0;
			nd->d_file_type = 0;

			de->d_ino = htole64(ino);
			de->d_rec_len = htole16((uint16_t)need);
			de->d_name_len = (uint8_t)nl;
			de->d_file_type = 1;
			memcpy(de->d_name, name, nl);
			break;
		}
		off += rec;
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		uint8_t *sub = blk + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;

		memcpy(sub, flat + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
		rs_encode_subblock(rs, sub, BEAMFS_SUBBLOCK_DATA,
				   sub + BEAMFS_SUBBLOCK_DATA);
	}
	pwrite_at(fd, blk_off, blk, sizeof(blk));

	/* The root now has one more entry pointing out of it. */
	root.i_nlink = htole16((uint16_t)(le16toh(root.i_nlink)));
	write_inode(fd, sb, 1, &root, rs);
}

/* ---- the cases ---- */

/*
 * A volume nothing has touched.
 *
 * The floor. A checker that cannot agree that a fresh mkfs is clean has
 * nothing useful to say about anything else -- and this case failed
 * until an hour ago, when the reader decoded the superblock as if it
 * were a data block and pass 4 gave up without saying so.
 */
static void case_pristine(void)
{
	struct verdict v;

	fresh();
	v = check();
	report("pristine volume", v.rc == 0 && v.lost == 0 && v.dangling == 0
	       && v.pass4_ran && !v.sb_bad,
	       "rc=%d lost=%ld dangling=%ld orphans=%ld free_dirents=%ld pass4=%s",
	       v.rc, v.lost, v.dangling, v.orphans, v.free_dirents, v.pass4_ran ? "ran" : "SKIPPED");
}

/*
 * Nine consecutive bytes flipped in one block.
 *
 * What a heavy ion through a die produces: neighbouring cells, not
 * scattered ones. With a codeword owning 239 consecutive bytes they
 * all land in the same one, which corrects eight, and the subblock is
 * lost -- "subblock N beyond correction", every time it has appeared.
 *
 * This case says what the checker makes of that on a real volume,
 * which is the number that matters. It is expected to report damage
 * until the layout groups data and parity; the point is that the
 * expectation is written down and checked rather than remembered.
 */
static void case_nine_byte_burst(void)
{
	struct beamfs_super_block sb;
	struct verdict v;
	uint8_t blk[BEAMFS_BLOCK_SIZE];
	uint64_t where;
	int fd;
	size_t i;

	fresh();

	fd = open(image, O_RDWR);
	if (fd < 0)
		die("cannot open %s", image);
	if (pread(fd, &sb, sizeof(sb), 0) != (ssize_t)sizeof(sb))
		die("cannot read the superblock");

	/*
	 * The first block of the inode table.
	 *
	 * pass 3 decodes every inode on the volume, so a burst there is
	 * one the checker actually meets -- unlike a data block, whose
	 * contents it never reads.
	 */
	where = le64toh(sb.s_inode_table_blk);
	if (pread(fd, blk, sizeof(blk), (off_t)where * BEAMFS_BLOCK_SIZE)
	    != (ssize_t)sizeof(blk))
		die("cannot read block %llu", (unsigned long long)where);

	/*
	 * Nine bytes inside the first inode's RS-covered area. An inode
	 * is 172 bytes of data with 16 of parity, so nine consecutive
	 * flips are one more than it corrects.
	 */
	for (i = 0; i < 9; i++)
		blk[8 + i] ^= 0xff;

	if (pwrite(fd, blk, sizeof(blk), (off_t)where * BEAMFS_BLOCK_SIZE)
	    != (ssize_t)sizeof(blk))
		die("cannot write block %llu", (unsigned long long)where);
	close(fd);

	v = check();
	/*
	 * Expected to report damage, and it will keep doing so.
	 *
	 * An inode is 172 bytes under one codeword: there is nothing to
	 * interleave and eight symbols is all it corrects, wherever they
	 * fall. Interleaving takes a data block from nine bytes to 139
	 * and leaves this at nine.
	 *
	 * So the case asserts the limit rather than hoping it goes away.
	 * It fails the day an inode is protected some other way -- a
	 * replica, most likely -- and that is when it should be
	 * revisited.
	 */
	report("nine bytes in an inode: still fatal, as expected",
	       v.rc != 0,
	       "rc=%d -- one codeword over 172 bytes, eight symbols corrected",
	       v.rc);
}

/*
 * One block marked used that nothing points to.
 *
 * The exact shape of what generic/464 is said to produce. If the
 * checker cannot count one, no count it has ever produced means
 * anything.
 */
static void case_one_leak(void)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);
	/* Well past the canary and anything mkfs writes. */
	set_block_used(fd, &sb, sb.s_data_start_blk + 100, true, rs);
	close(fd);
	rs_free(rs);

	v = check();
	report("one leaked block", v.lost == 1 && v.dangling == 0,
	       "expected lost=1 dangling=0, got lost=%ld dangling=%ld rc=%d",
	       v.lost, v.dangling, v.rc);
}

/*
 * An indirect block written without its parity keeps its children.
 *
 * The parity region describes indirect blocks from outside them, so a
 * block can reach the medium while its slot stays zero -- which is not
 * damage to the block and says nothing about what it holds.
 *
 * fsck_read_indirect answered that case by returning UNDESCRIBED
 * before filling the caller's pointer array, and every caller tested
 * only for UNCORRECTABLE, so the walk went on through 4 KiB of
 * uninitialised stack. Every child of such a block was then reported
 * as marked used and referenced by nothing. On one generic/083 volume
 * that was 266 blocks of 266, and pass 6 walked 2 inodes where it
 * should have walked 2106.
 *
 * Here the block is written directly rather than through
 * write_indirect, which is what leaves the region holding zeroes.
 */
static void case_indirect_without_parity(void)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct beamfs_inode in;
	uint64_t ptrs[BEAMFS_INDIRECT_PTRS];
	uint8_t raw[BEAMFS_BLOCK_SIZE];
	struct verdict v;
	uint64_t data, ind;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);

	data = sb.s_data_start_blk + 300;
	ind  = sb.s_data_start_blk + 301;

	set_block_used(fd, &sb, data, true, rs);
	set_block_used(fd, &sb, ind, true, rs);

	/* The block alone: the parity region keeps the zeroes mkfs left. */
	memset(ptrs, 0, sizeof(ptrs));
	ptrs[0] = data;
	memset(raw, 0, sizeof(raw));
	memcpy(raw, ptrs, sizeof(ptrs));
	pwrite_at(fd, (off_t)ind * BEAMFS_BLOCK_SIZE, raw, sizeof(raw));

	memset(&in, 0, sizeof(in));
	in.i_mode = 0x8000 | 0644;
	in.i_nlink = 1;
	in.i_size = BEAMFS_BLOCK_SIZE;
	in.i_indirect = ind;
	write_inode(fd, &sb, 12, &in, rs);
	link_into_root(fd, &sb, 12, "noparity", rs);
	close(fd);
	rs_free(rs);

	v = check();
	report("an indirect block with no parity keeps its children and is damage",
	       v.lost == 0 && v.dangling == 0 && v.rc == 4,
	       "expected lost=0 dangling=0 rc=4, got lost=%ld dangling=%ld rc=%d",
	       v.lost, v.dangling, v.rc);
}

/* Seventeen, to catch a checker that reports presence rather than count. */
static void case_many_leaks(void)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	int fd, i;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);
	for (i = 0; i < 17; i++)
		set_block_used(fd, &sb, sb.s_data_start_blk + 200 + i, true, rs);
	close(fd);
	rs_free(rs);

	v = check();
	report("seventeen leaked blocks", v.lost == 17,
	       "expected lost=17, got lost=%ld", v.lost);
}

/*
 * A referenced block the bitmap calls free.
 *
 * The other direction, and the more dangerous one: the allocator may
 * hand out a block a file is using. A checker that only looks for leaks
 * misses it entirely.
 */
static void case_dangling(void)
{
	struct beamfs_super_block sb;
	struct beamfs_inode in;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	uint64_t victim;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);

	/* Inode 2 is the canary mkfs writes: a real file with a real
	 * block, so its pointer is one the walk will follow.
	 */
	/*
	 * Build the case rather than borrow it: the canary sits below
	 * data_start and is outside the bitmap, so freeing "its" block
	 * changes nothing pass 4 can see. Point a spare inode at a real
	 * data block, mark that block used, and then free it -- a
	 * reference to a block the bitmap calls free, made on purpose.
	 */
	victim = sb.s_data_start_blk + 400;
	memset(&in, 0, sizeof(in));
	in.i_mode = 0x8000 | 0644;   /* a regular file */
	in.i_nlink = 1;
	in.i_size = BEAMFS_DATA_INLINE_BYTES;
	in.i_direct[0] = victim;
	write_inode(fd, &sb, 3, &in, rs);
	link_into_root(fd, &sb, 3, "victim", rs);
	set_block_used(fd, &sb, victim, false, rs);
	close(fd);
	rs_free(rs);

	v = check();
	report("dangling reference", v.dangling == 1,
	       "expected dangling=1, got dangling=%ld lost=%ld", v.dangling, v.lost);
}



/*
 * Write an indirect block and the parity that describes it.
 *
 * The parity lives in its own region, not inside the block, so writing
 * the block alone leaves the region holding zeroes -- and a case that
 * then flips a bit is testing a block nothing protects rather than the
 * recovery it meant to test. An earlier version did exactly that and
 * reported a failure against a checker that had nothing to work with.
 */
static void write_indirect(int fd, const struct beamfs_super_block *sb,
			   uint64_t blk, const uint64_t *ptrs,
			   struct rs_codec *rs)
{
	uint8_t raw[BEAMFS_BLOCK_SIZE];
	uint8_t par[BEAMFS_BLOCK_SIZE];
	uint8_t flat[BEAMFS_DATA_INLINE_BYTES];
	uint64_t idx, region_blk;
	uint32_t off;
	size_t stride;
	unsigned int slots;
	unsigned int i;

	memset(raw, 0, sizeof(raw));
	memcpy(raw, ptrs, sizeof(uint64_t) * BEAMFS_INDIRECT_PTRS);
	pwrite_at(fd, (off_t)blk * BEAMFS_BLOCK_SIZE, raw, sizeof(raw));

	if (sb->s_ind_parity_blk == 0 || sb->s_ind_parity_len == 0)
		return;

	/*
	 * The fourth copy of this arithmetic -- kernel, mkfs, checker,
	 * here -- and the one that matters most, because the oracle is
	 * what says the checker is right. When it wrote slots the checker
	 * did not read, four of the twelve cases failed and the failure
	 * looked like a defect in the checker.
	 */
	if (sb->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		stride = BEAMFS_IND_PARITY_CRC_BYTES;
		slots  = BEAMFS_IND_PARITY_CRC_SLOTS;
	} else {
		stride = BEAMFS_IND_PARITY_RS_BYTES;
		slots  = BEAMFS_IND_PARITY_RS_SLOTS;
	}

	idx = blk - sb->s_data_start_blk;
	region_blk = sb->s_ind_parity_blk + idx / slots;
	off = (uint32_t)(idx % slots) * (uint32_t)stride;
	if (off + stride > BEAMFS_DATA_INLINE_BYTES ||
	    region_blk >= sb->s_ind_parity_blk + sb->s_ind_parity_len)
		return;

	pread_at(fd, (off_t)region_blk * BEAMFS_BLOCK_SIZE, par, sizeof(par));

	/* The region block carries its own FEC: decode, edit, re-encode. */
	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		uint8_t *sub = par + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;
		int positions[BEAMFS_RS_PARITY / 2];

		rs_decode_subblock(rs, sub, BEAMFS_SUBBLOCK_DATA,
				   sub + BEAMFS_SUBBLOCK_DATA, positions);
		memcpy(flat + (size_t)i * BEAMFS_SUBBLOCK_DATA, sub,
		       BEAMFS_SUBBLOCK_DATA);
	}

	if (sb->s_ind_parity_mode == BEAMFS_IND_PARITY_CRC) {
		uint32_t *slot = (uint32_t *)(flat + off);

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			slot[i] = crc32(raw + (size_t)i * BEAMFS_SUBBLOCK_DATA,
					BEAMFS_SUBBLOCK_DATA);
	} else {
		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			rs_encode_subblock(rs,
					   raw + (size_t)i * BEAMFS_SUBBLOCK_DATA,
					   BEAMFS_SUBBLOCK_DATA,
					   flat + off + (size_t)i * BEAMFS_RS_PARITY);
	}

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		uint8_t *sub = par + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;

		memcpy(sub, flat + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
		rs_encode_subblock(rs, sub, BEAMFS_SUBBLOCK_DATA,
				   sub + BEAMFS_SUBBLOCK_DATA);
	}
	pwrite_at(fd, (off_t)region_blk * BEAMFS_BLOCK_SIZE, par, sizeof(par));
}

/*
 * Flip one bit inside a named field of an inode, on the medium.
 *
 * Written after the inode is complete, so the CRC and parity on disk
 * describe the undamaged inode and the flip is genuine medium damage --
 * recoverable, and the checker must recover it rather than report it.
 */
static void flip_bit_in_field(int fd, const struct beamfs_super_block *sb,
			      uint64_t ino, size_t field_off)
{
	uint32_t per = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	uint64_t blk = sb->s_inode_table_blk + (ino - 1) / per;
	uint64_t idx = (ino - 1) % per;
	off_t off = (off_t)blk * BEAMFS_BLOCK_SIZE
		  + (off_t)idx * sizeof(struct beamfs_inode)
		  + (off_t)field_off;
	uint8_t byte;

	pread_at(fd, off, &byte, 1);
	byte ^= 0x01;
	pwrite_at(fd, off, &byte, 1);
}

/*
 * A file whose pointer field @which is the one under test.
 *
 * Blocks are marked used so the volume is healthy in every respect
 * except the bit that gets flipped: a case that leaves a real leak
 * behind cannot tell a phantom from the genuine article.
 */
static uint64_t build_file(int fd, const struct beamfs_super_block *sb,
			   uint64_t ino, uint64_t base, int level,
			   struct rs_codec *rs)
{
	struct beamfs_inode in;
	uint64_t ind, child, data;
	uint64_t ptrs[BEAMFS_INDIRECT_PTRS];
	int i;

	memset(&in, 0, sizeof(in));
	in.i_mode = 0x8000 | 0644;
	in.i_nlink = 1;
	in.i_size = BEAMFS_DATA_INLINE_BYTES;

	if (level == 0) {
		data = base;
		in.i_direct[0] = data;
		set_block_used(fd, sb, data, true, rs);
		write_inode(fd, sb, ino, &in, rs);
		return data;
	}

	/*
	 * One indirect block holding one data pointer, and for the
	 * deeper levels an indirect block holding a pointer to that.
	 * The shape matters more than the size: what is being tested is
	 * whether a flip in the field naming the top of this is caught.
	 */
	data = base;
	set_block_used(fd, sb, data, true, rs);

	ind = base + 1;
	memset(ptrs, 0, sizeof(ptrs));
	ptrs[0] = data;
	write_indirect(fd, sb, ind, ptrs, rs);
	set_block_used(fd, sb, ind, true, rs);

	child = ind;
	for (i = 1; i < level; i++) {
		uint64_t up = base + 1 + (uint64_t)i;

		memset(ptrs, 0, sizeof(ptrs));
		ptrs[0] = child;
		write_indirect(fd, sb, up, ptrs, rs);
		set_block_used(fd, sb, up, true, rs);
		child = up;
	}

	if (level == 1)
		in.i_indirect = child;
	else if (level == 2)
		in.i_dindirect = child;
	else
		in.i_tindirect = child;

	write_inode(fd, sb, ino, &in, rs);
	return child;
}

/*
 * One bit flipped in one pointer field, on an otherwise healthy volume.
 *
 * The checksum has to notice, Reed-Solomon has to correct it, and the
 * checker has to report nothing. Run once per field: a coverage range
 * that is right for i_direct and wrong for i_tindirect passes every
 * test that only looks at i_direct.
 */
static void case_pointer_field(const char *label, size_t field_off, int level,
			       uint64_t base)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);

	build_file(fd, &sb, 3, sb.s_data_start_blk + base, level, rs);
	link_into_root(fd, &sb, 3, "victim", rs);
	flip_bit_in_field(fd, &sb, 3, field_off);
	close(fd);
	rs_free(rs);

	v = check();
	report(label, v.lost == 0 && v.dangling == 0,
	       "one recoverable bit in this field: expected 0/0, got lost=%ld dangling=%ld",
	       v.lost, v.dangling);
}

/*
 * A correctable error in an inode the checker must see through.
 *
 * This is the case the two defects would have failed. pass 4 read
 * inodes without correcting them, so a single flipped bit gave it a
 * pointer into nowhere: it marked a block nobody uses and reported the
 * canary's real block as lost. One bit, two phantom findings, and a
 * volume that is entirely healthy.
 */
static void case_correctable_inode(void)
{
	struct beamfs_super_block sb;
	struct beamfs_inode in;
	struct verdict v;
	uint32_t per;
	uint64_t blk, idx;
	off_t off;
	uint8_t byte;
	int fd;

	struct rs_codec *rs = rs_init();

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);

	/* A file with one real, properly marked block: healthy in every
	 * respect except one bit of medium damage.
	 */
	memset(&in, 0, sizeof(in));
	in.i_mode = 0x8000 | 0644;
	in.i_nlink = 1;
	in.i_size = BEAMFS_DATA_INLINE_BYTES;
	in.i_direct[0] = sb.s_data_start_blk + 600;
	write_inode(fd, &sb, 3, &in, rs);
	link_into_root(fd, &sb, 3, "victim", rs);
	set_block_used(fd, &sb, in.i_direct[0], true, rs);
	rs_free(rs);

	per = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	blk = sb.s_inode_table_blk + (3 - 1) / per;
	idx = (3 - 1) % per;
	/* One bit inside i_direct[0], within the RS-covered range, and
	 * left uncorrected on disk: the medium is damaged, the data is
	 * recoverable, and that is exactly the situation this
	 * filesystem exists for.
	 */
	off = (off_t)blk * BEAMFS_BLOCK_SIZE + (off_t)idx * sizeof(struct beamfs_inode)
	    + offsetof(struct beamfs_inode, i_direct);
	pread_at(fd, off, &byte, 1);
	byte ^= 0x01;
	pwrite_at(fd, off, &byte, 1);
	close(fd);

	v = check();
	report("correctable inode error", v.lost == 0 && v.dangling == 0,
	       "a healthy volume with one recoverable bit: expected 0/0, got lost=%ld dangling=%ld",
	       v.lost, v.dangling);
}

/*
 * A correctable error in an indirect block itself.
 *
 * Not in the pointer to it -- in the block. The walk read these with a
 * bare read() and no check of any kind, then followed every pointer it
 * found, so one flipped bit sent it somewhere else and orphaned the
 * subtree beneath: hundreds of blocks reported lost with nothing wrong
 * with any of them.
 *
 * An earlier version of this case looked for a file on a fresh volume
 * large enough to have an indirect block, found none, and reported
 * skipped on every run. Skipped is not passed. It builds one.
 */
static void case_correctable_indirect(void)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	uint64_t ind;
	uint8_t byte;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);

	ind = build_file(fd, &sb, 3, sb.s_data_start_blk + 900, 1, rs);
	link_into_root(fd, &sb, 3, "big", rs);

	/*
	 * The indirect block's parity lives in the parity region, not in
	 * the block, so flipping a byte here is damage the region can
	 * describe -- if the checker consults it.
	 */
	pread_at(fd, (off_t)ind * BEAMFS_BLOCK_SIZE, &byte, 1);
	byte ^= 0x01;
	pwrite_at(fd, (off_t)ind * BEAMFS_BLOCK_SIZE, &byte, 1);
	close(fd);
	rs_free(rs);

	v = check();
	report("correctable indirect block", v.lost == 0 && v.dangling == 0,
	       "expected 0/0, got lost=%ld dangling=%ld", v.lost, v.dangling);
}

/*
 * A leak and a dangling reference at once.
 *
 * Real damage is not tidy, and a checker that handles either alone but
 * confuses the two when both are present is worse than one that fails
 * outright: its numbers look reasonable.
 */
static void case_both(void)
{
	struct beamfs_super_block sb;
	struct beamfs_inode in;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);
	/* One reference to a free block, and one used block nobody
	 * points at, on the same volume.
	 */
	memset(&in, 0, sizeof(in));
	in.i_mode = 0x8000 | 0644;
	in.i_nlink = 1;
	in.i_size = BEAMFS_DATA_INLINE_BYTES;
	in.i_direct[0] = sb.s_data_start_blk + 500;
	write_inode(fd, &sb, 3, &in, rs);
	link_into_root(fd, &sb, 3, "victim", rs);
	set_block_used(fd, &sb, sb.s_data_start_blk + 300, true, rs);
	close(fd);
	rs_free(rs);

	v = check();
	report("one of each at once", v.lost == 1 && v.dangling == 1,
	       "expected lost=1 dangling=1, got lost=%ld dangling=%ld",
	       v.lost, v.dangling);
}

/*
 * An inode pointing outside the device.
 *
 * A checker that follows it reads whatever is there, or crashes. It
 * must be counted as a bad pointer and skipped, and the volume's real
 * blocks must still be accounted for.
 */
static void case_out_of_range_pointer(void)
{
	struct beamfs_super_block sb;
	struct beamfs_inode in;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);
	memset(&in, 0, sizeof(in));
	in.i_mode = 0x8000 | 0644;
	in.i_nlink = 1;
	in.i_size = BEAMFS_DATA_INLINE_BYTES;
	in.i_direct[0] = sb.s_block_count + 1000000;
	write_inode(fd, &sb, 3, &in, rs);
	link_into_root(fd, &sb, 3, "victim", rs);
	close(fd);
	rs_free(rs);

	v = check();
	/* The canary's block is now unreferenced, so exactly one is
	 * expected -- and the checker must survive to say so.
	 */
	/* Nothing else is wrong with the volume, so a checker that
	 * skips the bad pointer and keeps going reports nothing.
	 */
	report("pointer past the end of the device", v.rc >= 0 && v.lost == 0,
	       "expected to survive with lost=0, got rc=%d lost=%ld", v.rc, v.lost);
}


/*
 * A name pointing at an inode that was never written.
 *
 * generic/076 came back with five of these and nothing in the oracle
 * could produce one, so there was no way to tell whether the checker
 * had found a real defect or invented a category. The entry goes into
 * the root; the inode it names keeps i_mode == 0, which is what pass 6
 * calls free.
 */
static void case_dirent_to_free_inode(void)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);
	/* Named, never written: the inode stays as mkfs left it. */
	link_into_root(fd, &sb, 2048, "ghost", rs);
	close(fd);
	rs_free(rs);

	v = check();
	report("a name for an inode that does not exist",
	       v.free_dirents == 1 && v.orphans == 0,
	       "expected free_dirents=1 orphans=0, got %ld/%ld rc=%d",
	       v.free_dirents, v.orphans, v.rc);
}

/* Five of them, because 076 reported five and a count that is only
 * ever one proves nothing about a count.
 */
static void case_five_dirents_to_free_inodes(void)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	char name[16];
	int fd, i;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);
	for (i = 0; i < 5; i++) {
		snprintf(name, sizeof(name), "ghost%d", i);
		link_into_root(fd, &sb, 2048 + (uint64_t)i, name, rs);
	}
	close(fd);
	rs_free(rs);

	v = check();
	report("five names for inodes that do not exist",
	       v.free_dirents == 5,
	       "expected free_dirents=5, got %ld rc=%d", v.free_dirents, v.rc);
}

/*
 * An inode in service that no directory names.
 *
 * The other half of what 076 reported. build_file writes the inode and
 * marks its block used, so nothing leaks: the only thing wrong is that
 * no name reaches it.
 */
static void case_unreachable_inode(void)
{
	struct beamfs_super_block sb;
	struct rs_codec *rs = rs_init();
	struct verdict v;
	int fd;

	fresh();
	fd = open(image, O_RDWR);
	if (fd < 0 || !rs)
		die("cannot open %s", image);
	read_sb(fd, &sb);
	/* Written and its block accounted for, simply not linked. */
	build_file(fd, &sb, 2148, sb.s_data_start_blk + 300, 0, rs);
	close(fd);
	rs_free(rs);

	v = check();
	report("an inode no directory reaches",
	       v.orphans == 1 && v.lost == 0 && v.free_dirents == 0,
	       "expected orphans=1 lost=0 free_dirents=0, got %ld/%ld/%ld rc=%d",
	       v.orphans, v.lost, v.free_dirents, v.rc);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"usage: %s <fsck.beamfs> <mkfs.beamfs> [image]\n"
			"\n"
			"Builds a volume whose state is known, damages it in one\n"
			"known way, and requires the checker to report that and\n"
			"nothing else.\n", argv[0]);
		return 2;
	}
	fsck_bin = argv[1];
	mkfs_bin = argv[2];
	image = argc > 3 ? argv[3] : "/tmp/fsck-oracle.img";
	only  = argc > 4 ? argv[4] : NULL;

	printf("fsck oracle: %s against %s\n\n", fsck_bin, image);

	/* One case only, when named: an oracle that overwrites its image
	 * at every case leaves the last one behind, and the one worth
	 * looking at is whichever failed.
	 */
	if (only) {
		if (!strcmp(only, "pristine"))
			case_pristine();

		if (!strcmp(only, "leak"))
			case_one_leak();

		if (!strcmp(only, "noparity"))
			case_indirect_without_parity();


		if (!strcmp(only, "many"))
			case_many_leaks();

		if (!strcmp(only, "dangling"))
			case_dangling();

		if (!strcmp(only, "both"))
			case_both();

		if (!strcmp(only, "inode"))
			case_correctable_inode();

		if (!strcmp(only, "indirect"))
			case_correctable_indirect();

		if (!strcmp(only, "direct"))
			case_pointer_field("bit in i_direct[0]",
					   offsetof(struct beamfs_inode, i_direct), 0, 700);
		if (!strcmp(only, "ind1"))
			case_pointer_field("bit in i_indirect",
					   offsetof(struct beamfs_inode, i_indirect), 1, 720);
		if (!strcmp(only, "ind2"))
			case_pointer_field("bit in i_dindirect",
					   offsetof(struct beamfs_inode, i_dindirect), 2, 740);
		if (!strcmp(only, "ind3"))
			case_pointer_field("bit in i_tindirect",
					   offsetof(struct beamfs_inode, i_tindirect), 3, 760);
		if (!strcmp(only, "range"))
			case_out_of_range_pointer();

		printf("\n%u case(s), %u failed\n", cases_run, cases_failed);
		return cases_failed > 0;
	}

	case_pristine();
	case_nine_byte_burst();
	case_one_leak();
	case_indirect_without_parity();
	case_many_leaks();
	case_dangling();
	case_both();
	case_correctable_inode();

	/*
	 * Every pointer field, not just the one that was convenient.
	 * The offsets come from the struct rather than from arithmetic
	 * here, so a field moving does not silently stop being tested.
	 */
	case_pointer_field("bit in i_direct[0]",
			   offsetof(struct beamfs_inode, i_direct), 0, 700);
	case_pointer_field("bit in i_indirect",
			   offsetof(struct beamfs_inode, i_indirect), 1, 720);
	case_pointer_field("bit in i_dindirect",
			   offsetof(struct beamfs_inode, i_dindirect), 2, 740);
	case_pointer_field("bit in i_tindirect",
			   offsetof(struct beamfs_inode, i_tindirect), 3, 760);

	case_correctable_indirect();
	case_out_of_range_pointer();
	case_dirent_to_free_inode();
	case_five_dirents_to_free_inodes();
	case_unreachable_inode();

	printf("\n%u case(s), %u failed\n", cases_run, cases_failed);
	if (cases_failed)
		printf("a checker that misreports known damage cannot be used to\n"
		       "judge unknown damage; every number it has produced is suspect\n");
	return cases_failed > 0;
}
