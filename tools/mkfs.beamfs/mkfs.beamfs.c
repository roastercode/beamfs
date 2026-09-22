/* SPDX-License-Identifier: GPL-2.0-only
 * mkfs.beamfs -- format a block device or image as BEAMFS
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Usage: mkfs.beamfs [-N inodes] <device_or_image>
 *
 * Layout:
 *   Block 0    : superblock
 *   Block 1..N : inode table
 *   Block N+1  : bitmap block (RS FEC protected)
 *   Block N+2  : root directory data
 *   Block N+3+ : data blocks
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#include <stddef.h>
#include <assert.h>
#include <getopt.h>
#include <dirent.h>

/*
 * The on-disk format, from the same header the kernel uses.
 *
 * mkfs kept its own copy of the structures and constants until
 * 2026-08-30, when three defects in a day came from the copies
 * drifting apart -- including this one marking the canary block
 * with an index it computed differently from everyone else.
 */
#include "beamfs_format.h"
#include "rs_decode.h"


/*
 * Indirection parity, chosen here rather than fixed, because the right
 * answer depends on where the volume is going. Must match
 * enum beamfs_ind_parity_mode in beamfs.h.
 */
#define BEAMFS_BITMAP_BITS_PER_BLOCK (BEAMFS_BITMAP_SUBBLOCKS * BEAMFS_SUBBLOCK_DATA * 8)  /* 30592 bits per bitmap block */

/* RS FEC constants (must match kernel beamfs.h) */

/* Superblock RS layout (stage 3 item 4, v4 format, must match kernel beamfs.h) */
                                          /* index in s_pad[]: 1175     */

/* Format version (must match kernel beamfs.h) */

/* Data protection scheme values (must match kernel beamfs.h) */

/* RS coverage on the inode: bytes [0, offsetof(i_reserved)) = 172 */

/* INLINE scheme constants (must match kernel beamfs.h) */

/* DATA_CSUM descriptor in block tail pad (format-v6); mirrors beamfs.h */
/* DATA_SELFID (format-v6): identity digest in the second half of the pad. */
/*
 * What an untouched data block holds under --poison.
 *
 * 0xbe 0xef repeated: not zero, which is what a fresh image gives and
 * what a hole reads as; not 0xcd, which is the kernel's own poison for
 * unwritten memory and would confuse the two; and not a value any test
 * writes -- fsx fills with its own sequence numbers and generic/013
 * with text.
 *
 * A block still holding it was allocated and never written. A block
 * holding anything else was written to, and what it holds says by
 * whom.
 */
#define BEAMFS_MKFS_POISON_A 0xbe
#define BEAMFS_MKFS_POISON_B 0xef

/*
 * Write the pattern over every data block.
 *
 * A gigabyte volume is a gigabyte to write and the suite makes
 * hundreds of them, so this is off unless asked for. It is asked for
 * when a campaign is chasing blocks that are allocated and never
 * written, which is what generic/269 and generic/464 leave behind.
 */
static int poison_data_area(int fd, uint64_t data_start, uint64_t nblocks)
{
	static uint8_t buf[1u << 20];		/* a mebibyte at a time */
	uint64_t done = 0;
	size_t i;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (i & 1) ? BEAMFS_MKFS_POISON_B : BEAMFS_MKFS_POISON_A;

	while (done < nblocks) {
		uint64_t left = nblocks - done;
		size_t want = sizeof(buf);
		ssize_t got;

		if (left * BEAMFS_BLOCK_SIZE < want)
			want = (size_t)(left * BEAMFS_BLOCK_SIZE);

		got = pwrite(fd, buf, want,
			     (off_t)(data_start + done) * BEAMFS_BLOCK_SIZE);
		if (got != (ssize_t)want) {
			fprintf(stderr,
				"mkfs.beamfs: cannot poison block %llu: %s\n",
				(unsigned long long)(data_start + done),
				strerror(errno));
			return -1;
		}
		done += want / BEAMFS_BLOCK_SIZE;
	}
	return 0;
}

/* Reserved inode numbers, must match kernel beamfs.h. */

/* Conformance fixture (canary block) -- v4 INLINE only */

/* Minimal CRC32 for userspace mkfs */
static uint32_t crc32_table[256];
static int crc32_init_done = 0;

static void crc32_init(void)
{
	uint32_t poly = 0xEDB88320;
	for (int i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int j = 0; j < 8; j++)
			c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
		crc32_table[i] = c;
	}
	crc32_init_done = 1;
}

/*
 * crc32_internal -- compute CRC32, returning the raw internal state (no XOR).
 * seed: initial internal state (0xFFFFFFFF for first block, carry for chaining)
 */
static uint32_t crc32_internal(uint32_t seed, const void *buf, size_t len)
{
	if (!crc32_init_done) crc32_init();
	uint32_t c = seed;
	const uint8_t *p = buf;
	while (len--)
		c = crc32_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return c;
}

static uint32_t crc32(const void *buf, size_t len)
{
	return crc32_internal(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

/*
 * stamp_data_csum -- write the DATA_CSUM descriptor into the 16-byte block
 * tail pad after RS encode, matching the kernel beamfs_inline_stamp_tail_pad.
 * The pad is already zero from the caller's block memset; when enabled, stamp
 * csum_type = CRC32 and crc32() of the contiguous 3824-byte payload (the same
 * bytes the kernel read path recomputes over). Little-endian byte store, no
 * host-endianness assumption. See format-v6.md.
 */
/*
 * Where the descriptor goes, and how much it covers.
 *
 * A capsule keeps it at 3808, inside the coded area, over 3808 bytes
 * of data. The alternating layout keeps it at 4080, outside every
 * codeword, over 3824. Two offsets and two lengths, and getting them
 * crossed writes a descriptor the reader will not find.
 */
static void stamp_data_csum_at(uint8_t *block, const uint8_t *payload,
			       int enabled, uint64_t ino, uint64_t iblock,
			       size_t csum_off, size_t selfid_off,
			       size_t covered);

static void stamp_data_csum(uint8_t *block, const uint8_t *payload, int enabled,
			    uint64_t ino, uint64_t iblock)
{
	stamp_data_csum_at(block, payload, enabled, ino, iblock,
			   BEAMFS_DATA_CSUM_TYPE_OFF,
			   BEAMFS_DATA_SELFID_OFF,
			   BEAMFS_DATA_INLINE_BYTES);
}

static void stamp_capsule_csum(uint8_t *block, const uint8_t *payload,
			       int enabled, uint64_t ino, uint64_t iblock)
{
	stamp_data_csum_at(block, payload, enabled, ino, iblock,
			   BEAMFS_CAPSULE_CSUM_OFF,
			   BEAMFS_CAPSULE_SELFID_OFF,
			   BEAMFS_CAPSULE_DATA_BYTES);
}

static void stamp_data_csum_at(uint8_t *block, const uint8_t *payload,
			       int enabled, uint64_t ino, uint64_t iblock,
			       size_t csum_off, size_t selfid_off,
			       size_t covered)
{
	uint32_t c;
	uint64_t id;
	uint32_t lo, hi;
	uint8_t  le[8];
	int      k;

	if (!enabled)
		return;
	c = crc32(payload, covered);
	block[csum_off]     = BEAMFS_CSUM_CRC32;
	block[csum_off + 4] = (uint8_t)(c & 0xFF);
	block[csum_off + 5] = (uint8_t)((c >> 8) & 0xFF);
	block[csum_off + 6] = (uint8_t)((c >> 16) & 0xFF);
	block[csum_off + 7] = (uint8_t)((c >> 24) & 0xFF);

	/*
	 * DATA_SELFID: same digest the kernel computes in
	 * beamfs_data_selfid(), two CRC32s over the little-endian encodings
	 * of ino and iblock. Written here so a freshly formatted volume is
	 * readable by a kernel that verifies block identity.
	 */
	for (k = 0; k < 8; k++)
		le[k] = (uint8_t)((ino >> (8 * k)) & 0xFF);
	lo = crc32(le, 8);
	for (k = 0; k < 8; k++)
		le[k] = (uint8_t)((iblock >> (8 * k)) & 0xFF);
	hi = crc32(le, 8);
	id = ((uint64_t)hi << 32) | lo;
	for (k = 0; k < 8; k++)
		block[selfid_off + k] = (uint8_t)((id >> (8 * k)) & 0xFF);
}

/* On-disk structures (must match kernel beamfs.h) */

/*
 * Electromagnetic Resilience Journal entry -- 40 bytes (v4 format).
 *
 * Stage 3 item 4 introduces the per-event Shannon entropy estimate
 * as the forensic discriminator between Family A (Poisson background
 * SEU) and Family B (correlated burst). mkfs writes a fresh journal
 * with all entries zero-initialized; the kernel populates entries on
 * RS correction events at runtime.
 *
 * Layout invariants:
 *   - 40 bytes total, packed, no implicit padding
 *   - re_reserved and re_pad MUST be zero on write (sentinels)
 *   - re_crc32 covers bytes [0..32) excluding itself and re_pad
 *
 * Must match struct beamfs_rs_event in kernel beamfs.h byte-for-byte.
 */

/*
 * crc32_sb -- CRC32 over superblock fields matching beamfs_crc32_sb() in kernel.
 *
 * Coverage (computed at compile time from struct layout):
 *   region A: [0, offsetof(s_crc32))         -- magic, counters,
 *                                               version, flags
 *   region B: [offsetof(s_uuid), offsetof(s_pad)) -- uuid, label,
 *                                               RS journal,
 *                                               bitmap_blk, features,
 *                                               protection scheme
 * Total coverage: BEAMFS_SB_RS_COVERAGE_BYTES, asserted at compile
 * time by static_assert in the matching staging helper.
 *
 * Must match the kernel implementation byte-for-byte so that
 * superblocks formatted by mkfs validate at mount time.
 */
static uint32_t crc32_sb(const struct beamfs_super_block *sb)
{
	const uint8_t *base = (const uint8_t *)sb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);
	uint32_t c;

	static_assert(sizeof(((struct beamfs_super_block *)0)->s_crc32) == 4,
		      "s_crc32 must be 4 bytes");

	c = crc32_internal(0xFFFFFFFF, base, off_crc32);
	c = crc32_internal(c, base + off_uuid, off_pad - off_uuid);
	return c ^ 0xFFFFFFFF;
}


/*
 * encode_rs_userspace -- single-subblock RS(255,239) shortened encoder.
 *
 * Exact reproduction of lib/reed_solomon encode_rs8() with parameters:
 *   init_rs(8, 0x187, fcr=0, prim=1, nroots=16)
 *
 * @data:     input data bytes (data_len)
 * @data_len: number of data bytes (must be in [1, 239])
 * @parity:   output parity (16 bytes)
 *
 * The kernel encode_rs8 supports shortened codes natively for any
 * data_len <= 239: the unwritten bytes are mathematically padded
 * with zero, no special handling needed in the LFSR.
 *
 * Used by rs_encode_super (data_len=211). rs_encode_bitmap (data_len=239)
 * and rs_encode_inode (data_len=172) carry their own LFSR copies for
 * historical reasons; they will converge here in a follow-up cleanup.
 */
static void encode_rs_userspace(const uint8_t *data, size_t data_len,
				uint8_t *parity)
{
	/*
	 * GF(2^8) with primitive polynomial 0x187, primitive element
	 * alpha=1, fcr=0, nroots=16. Tables built lazily on first call.
	 */
	static uint8_t alpha_to[256], index_of[256];
	static uint8_t genpoly[17];
	static int     rs_table_init = 0;
	uint8_t        par[16];
	size_t         i;

	if (!rs_table_init) {
		unsigned int sr = 1, j;

		/*
		 * GF(2^8) table init - kernel-canonical convention.
		 *
		 * Earlier versions of this encoder used a 256-iteration loop
		 * that re-overwrote the wrap-around point, producing tables
		 * mathematically incompatible with the kernel decoder (see
		 * Documentation/incidents/2026-05-11-rs-encoder-divergence.md).
		 * R19 mount-time RS recovery has only ever succeeded by chance
		 * on superblock CRC mismatch events.
		 *
		 * Aligned with rs_encode_bitmap and rs_encode_inode below,
		 * which were always correct (loop i<nn, alpha_to[nn]=0,
		 * index_of[0]=nn post-set).
		 */
		index_of[0] = 255;
		alpha_to[255] = 0;
		for (j = 0; j < 255; j++) {
			alpha_to[j] = (uint8_t)sr;
			index_of[sr] = (uint8_t)j;
			sr <<= 1;
			if (sr & 0x100)
				sr ^= 0x187;
			sr &= 0xff;
		}

		/* Build genpoly: prod_{r=fcr..fcr+nroots-1} (x - alpha^r) */
		genpoly[0] = 1;
		for (j = 0; j < 16; j++) {
			unsigned int k;
			genpoly[j + 1] = 1;
			for (k = j; k > 0; k--) {
				if (genpoly[k] != 0)
					genpoly[k] = genpoly[k - 1] ^
						alpha_to[(index_of[genpoly[k]] + j) % 255];
				else
					genpoly[k] = genpoly[k - 1];
			}
			genpoly[0] = alpha_to[(index_of[genpoly[0]] + j) % 255];
		}
		/* Convert to index form for encoder */
		for (j = 0; j <= 16; j++)
			genpoly[j] = index_of[genpoly[j]];

		rs_table_init = 1;
	}

	memset(par, 0, sizeof(par));

	for (i = 0; i < data_len; i++) {
		uint8_t feedback = index_of[data[i] ^ par[0]];
		unsigned int j;

		if (feedback != 255) {
			for (j = 0; j < 15; j++)
				par[j] = par[j + 1] ^
					alpha_to[(feedback + genpoly[15 - j]) % 255];
			par[15] = alpha_to[(feedback + genpoly[0]) % 255];
		} else {
			for (j = 0; j < 15; j++)
				par[j] = par[j + 1];
			par[15] = 0;
		}
	}

	memcpy(parity, par, 16);
}

/*
 * rs_encode_bitmap -- protect 239-byte subblocks with 16-byte RS parity.
 *
 * Exact reproduction of lib/reed_solomon encode_rs8() with parameters:
 *   init_rs(8, 0x187, fcr=0, prim=1, nroots=16)
 *
 * Uses index-form genpoly exactly as codec_init() builds it, and
 * the same LFSR feedback loop as encode_rs.c.
 *
 * Layout: [data0..238][par0..15][data239..477][par..] (16 subblocks)
 */
static void rs_encode_bitmap(uint8_t *block)
{
	/* GF(2^8) with primitive polynomial 0x187 */
	static const int nn = 255;
	uint16_t alpha_to[256], index_of[256];
	uint16_t genpoly[17]; /* index form, nroots+1 elements */
	int sr, i, j, root;

	/* Build GF tables */
	sr = 1;
	for (i = 0; i < nn; i++) {
		index_of[sr] = (uint16_t)i;
		alpha_to[i]  = (uint8_t)sr;
		sr <<= 1;
		if (sr & 256)
			sr ^= 0x187;
		sr &= nn;
	}
	alpha_to[nn] = 0;
	index_of[0]  = nn;

	/* Build generator polynomial in index form -- exactly codec_init() */
	/* fcr=0, prim=1: roots are alpha^0 .. alpha^15 */
	uint16_t gp[17];
	memset(gp, 0, sizeof(gp));
	gp[0] = 1;
	root = 0; /* fcr * prim */
	for (i = 0; i < 16; i++) {
		gp[i + 1] = 1;
		for (j = i; j > 0; j--) {
			if (gp[j] != 0) {
				int idx = (index_of[gp[j]] + root) % nn;
				gp[j] = gp[j - 1] ^ alpha_to[idx];
			} else {
				gp[j] = gp[j - 1];
			}
		}
		gp[0] = alpha_to[(index_of[gp[0]] + root) % nn];
		root += 1; /* += prim */
	}
	/* Convert to index form */
	for (i = 0; i <= 16; i++)
		genpoly[i] = index_of[gp[i]];

	/* Encode each subblock -- exactly encode_rs.c LFSR */
	for (int k = 0; k < BEAMFS_BITMAP_SUBBLOCKS; k++) {
		uint8_t  *data   = block + k * BEAMFS_SUBBLOCK_TOTAL;
		uint16_t  par[16];
		memset(par, 0, sizeof(par));

		for (i = 0; i < BEAMFS_SUBBLOCK_DATA; i++) {
			uint16_t fb = index_of[(data[i] ^ (uint8_t)par[0]) & nn];
			if (fb != (uint16_t)nn) {
				for (j = 1; j < 16; j++)
					par[j] ^= alpha_to[(fb + genpoly[16 - j]) % nn];
			}
			memmove(&par[0], &par[1], sizeof(uint16_t) * 15);
			par[15] = (fb != (uint16_t)nn)
				? alpha_to[(fb + genpoly[0]) % nn]
				: 0;
		}

		/* Write parity bytes after data */
		uint8_t *parity = data + BEAMFS_SUBBLOCK_DATA;
		for (j = 0; j < 16; j++)
			parity[j] = (uint8_t)par[j];
	}
}

/*
 * The same encoder, on a capsule.
 *
 * Two things differ and nothing else: where a codeword's symbols are
 * read from -- byte i*16 + j rather than j*255 + i -- and where its
 * parity goes, which is the block of 256 at offset 3824 rather than
 * the sixteen bytes after each subblock's data.
 *
 * The LFSR is copied rather than shared because this file keeps its
 * own Galois tables, built once above and thrown away with the
 * function. Sharing would mean hoisting them, which is a change to
 * code that has been right for a year to save twenty lines here.
 */
static void rs_encode_capsule(uint8_t *block)
{
	static const int nn = 255;
	uint16_t alpha_to[256], index_of[256];
	uint16_t genpoly[17];
	int sr, i, j, root;

	sr = 1;
	for (i = 0; i < nn; i++) {
		index_of[sr] = (uint16_t)i;
		alpha_to[i]  = (uint8_t)sr;
		sr <<= 1;
		if (sr & 256)
			sr ^= 0x187;
		sr &= 255;
	}
	index_of[0] = (uint16_t)nn;
	alpha_to[nn] = 0;

	genpoly[0] = 1;
	for (i = 0, root = 0; i < 16; i++, root++) {
		genpoly[i + 1] = 1;
		for (j = i; j > 0; j--) {
			if (genpoly[j] != 0)
				genpoly[j] = (uint16_t)(genpoly[j - 1] ^
					alpha_to[(index_of[genpoly[j]] + root) % nn]);
			else
				genpoly[j] = genpoly[j - 1];
		}
		genpoly[0] = alpha_to[(index_of[genpoly[0]] + root) % nn];
	}
	for (i = 0; i <= 16; i++)
		genpoly[i] = index_of[genpoly[i]];

	for (int k = 0; k < BEAMFS_DATA_INLINE_SUBBLOCKS; k++) {
		uint16_t par[16];
		uint8_t *parity;

		memset(par, 0, sizeof(par));

		for (i = 0; i < BEAMFS_SUBBLOCK_DATA; i++) {
			uint8_t  sym = block[(size_t)i *
					     BEAMFS_DATA_INLINE_SUBBLOCKS + k];
			uint16_t fb  = index_of[(sym ^ (uint8_t)par[0]) & nn];

			if (fb != (uint16_t)nn) {
				for (j = 1; j < 16; j++)
					par[j] ^= alpha_to[(fb + genpoly[16 - j]) % nn];
			}
			memmove(&par[0], &par[1], sizeof(uint16_t) * 15);
			par[15] = (fb != (uint16_t)nn)
				? alpha_to[(fb + genpoly[0]) % nn]
				: 0;
		}

		parity = block + BEAMFS_CAPSULE_PARITY_OFF
			       + (size_t)k * BEAMFS_RS_PARITY;
		for (j = 0; j < 16; j++)
			parity[j] = (uint8_t)par[j];
	}
}

/*
 * rs_encode_inode -- compute 16 bytes of RS parity over the first
 * BEAMFS_INODE_RS_DATA bytes of an inode buffer, write the parity into
 * the next 16 bytes (which are i_reserved[0..15] of the inode).
 *
 * Uses the same shortened-code convention as lib/reed_solomon: a
 * codeword shorter than 239 is mathematically equivalent to the full
 * codeword padded with zeros at the front. The LFSR loop is identical
 * to encode_rs.c; only the iteration count changes.
 *
 * inode_buf must be at least BEAMFS_INODE_RS_DATA + 16 bytes wide.
 */
static void rs_encode_inode(uint8_t *inode_buf)
{
	static const int nn = 255;
	uint16_t alpha_to[256], index_of[256];
	uint16_t genpoly[17];
	uint16_t gp[17];
	uint16_t par[16];
	int sr, i, j, root;

	/* GF(2^8) tables (same as rs_encode_bitmap) */
	sr = 1;
	for (i = 0; i < nn; i++) {
		index_of[sr] = (uint16_t)i;
		alpha_to[i]  = (uint8_t)sr;
		sr <<= 1;
		if (sr & 256)
			sr ^= 0x187;
		sr &= nn;
	}
	alpha_to[nn] = 0;
	index_of[0]  = nn;

	memset(gp, 0, sizeof(gp));
	gp[0] = 1;
	root = 0;
	for (i = 0; i < 16; i++) {
		gp[i + 1] = 1;
		for (j = i; j > 0; j--) {
			if (gp[j] != 0) {
				int idx = (index_of[gp[j]] + root) % nn;
				gp[j] = gp[j - 1] ^ alpha_to[idx];
			} else {
				gp[j] = gp[j - 1];
			}
		}
		gp[0] = alpha_to[(index_of[gp[0]] + root) % nn];
		root += 1;
	}
	for (i = 0; i <= 16; i++)
		genpoly[i] = index_of[gp[i]];

	/* LFSR encode over BEAMFS_INODE_RS_DATA bytes */
	memset(par, 0, sizeof(par));
	for (i = 0; i < (int)BEAMFS_INODE_RS_DATA; i++) {
		uint16_t fb = index_of[(inode_buf[i] ^ (uint8_t)par[0]) & nn];
		if (fb != (uint16_t)nn) {
			for (j = 1; j < 16; j++)
				par[j] ^= alpha_to[(fb + genpoly[16 - j]) % nn];
		}
		memmove(&par[0], &par[1], sizeof(uint16_t) * 15);
		par[15] = (fb != (uint16_t)nn)
			? alpha_to[(fb + genpoly[0]) % nn]
			: 0;
	}

	/* Write parity to bytes [BEAMFS_INODE_RS_DATA .. +16) */
	for (j = 0; j < 16; j++)
		inode_buf[BEAMFS_INODE_RS_DATA + j] = (uint8_t)par[j];
}



/*
 * sb_to_rs_staging -- serialize the CRC32 coverage of a superblock
 * into a contiguous staging buffer for RS encode/decode.
 *
 * Output buffer layout (BEAMFS_SB_RS_STAGING_BYTES bytes):
 *   [0, off_crc32)                              region A:
 *                                                 sb_bytes[0..off_crc32)
 *   [off_crc32, BEAMFS_SB_RS_COVERAGE_BYTES)     region B:
 *                                                 sb_bytes[off_uuid..off_pad)
 *   [BEAMFS_SB_RS_COVERAGE_BYTES,
 *    BEAMFS_SB_RS_STAGING_BYTES)                 zero pad to round up
 *                                                 to whole RS subblocks
 *
 * Field offsets are derived from struct layout via offsetof, so the
 * helper is invariant under future format extensions provided
 * BEAMFS_SB_RS_COVERAGE_BYTES is updated in lockstep. static_assert
 * below enforces buffer sizing at compile time.
 *
 * The s_crc32 field [off_crc32, off_uuid) is excluded, exactly as
 * crc32_sb() does. Same coverage on both protection layers.
 *
 * Must match the kernel beamfs_sb_to_rs_staging() byte-for-byte.
 */
static void sb_to_rs_staging(const struct beamfs_super_block *sb,
			     uint8_t staging[BEAMFS_SB_RS_STAGING_BYTES])
{
	const uint8_t *base = (const uint8_t *)sb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);

	static_assert(BEAMFS_SB_RS_STAGING_BYTES >= BEAMFS_SB_RS_COVERAGE_BYTES,
		      "staging buffer too small for coverage");

	memcpy(staging, base, off_crc32);
	memcpy(staging + off_crc32, base + off_uuid, off_pad - off_uuid);
	memset(staging + BEAMFS_SB_RS_COVERAGE_BYTES, 0,
	       BEAMFS_SB_RS_STAGING_BYTES - BEAMFS_SB_RS_COVERAGE_BYTES);
}

/*
 * rs_encode_super -- compute the RS parity of a superblock and store
 * the parity bytes (BEAMFS_SB_RS_PARITY_BYTES total) at the trailing
 * end of the on-disk block.
 *
 * BEAMFS_SB_RS_SUBBLOCKS shortened RS(255,239) subblocks of
 * BEAMFS_SB_RS_DATA_LEN data bytes each are encoded over the staging
 * buffer. Each subblock produces BEAMFS_RS_PARITY bytes of parity.
 * The parity is placed at on-disk offset BEAMFS_SB_RS_PARITY_OFFSET,
 * which corresponds to sb->s_pad[BEAMFS_SB_RS_S_PAD_INDEX]. The
 * trailing position is stable against future format extensions:
 * new fields go into s_pad before the parity zone, and the
 * S_PAD_INDEX define is recomputed from the struct layout in
 * lockstep.
 *
 * Invariant: caller must have populated all sb fields except
 * s_crc32 before calling. s_crc32 lies in
 * [offsetof(s_crc32), offsetof(s_uuid)) and is excluded from the
 * staging copy, so this helper is idempotent w.r.t. s_crc32.
 */
static void rs_encode_super(struct beamfs_super_block *sb)
{
	uint8_t staging[BEAMFS_SB_RS_STAGING_BYTES];
	uint8_t *parity_dst = sb->s_pad + BEAMFS_SB_RS_S_PAD_INDEX;
	unsigned int i;

	sb_to_rs_staging(sb, staging);

	for (i = 0; i < BEAMFS_SB_RS_SUBBLOCKS; i++) {
		encode_rs_userspace(staging + i * BEAMFS_SB_RS_DATA_LEN,
				    BEAMFS_SB_RS_DATA_LEN,
				    parity_dst + i * BEAMFS_RS_PARITY);
	}
}

static void write_block(int fd, uint64_t block, const void *buf)
{
	off_t off = (off_t)block * BEAMFS_BLOCK_SIZE;
	if (lseek(fd, off, SEEK_SET) < 0) {
		perror("lseek");
		exit(1);
	}
	if (write(fd, buf, BEAMFS_BLOCK_SIZE) != BEAMFS_BLOCK_SIZE) {
		perror("write");
		exit(1);
	}
}


/*
 * The inode's checksum, over the head and the block pointers both.
 *
 * Byte-identical to the kernel's beamfs_inode_crc and to fsck's
 * crc32_inode. Three implementations of one format is how pass 3 and
 * pass 4 came to disagree; three copies of one formula is the same
 * risk, so they are written the same way and named the same thing.
 */
static uint32_t crc32_inode(const struct beamfs_inode *raw)
{
	uint8_t staging[BEAMFS_INODE_CRC_BYTES];

	memcpy(staging, raw, BEAMFS_INODE_CRC_HEAD_LEN);
	memcpy(staging + BEAMFS_INODE_CRC_HEAD_LEN,
	       (const uint8_t *)raw + BEAMFS_INODE_CRC_TAIL_OFF,
	       BEAMFS_INODE_CRC_TAIL_LEN);
	return crc32(staging, sizeof(staging));
}


/*
 * Indirect-block parity, kept in the region rather than in the block.
 *
 * A data or indirect block is 4096 bytes of payload with no room left
 * for its own parity, so the parity for every block past data_start
 * lives in a separate region: sixteen subblocks' worth per block, at an
 * offset computed from the block number. beamfs_ind_parity_update in
 * the kernel does the same arithmetic; this has to match it byte for
 * byte or every block reads as damaged.
 *
 * Called for every indirect block mkfs writes. Data blocks do not need
 * it -- their protection is inline, sixteen subblocks of 239 bytes each
 * followed by their own parity -- but indirect blocks are 512 flat
 * pointers with no inline room, which is why the region exists at all.
 */
/*
 * The codec, built once. rs_init allocates tables; doing it per slot
 * would dominate the runtime of a large mkfs.
 */
static struct rs_codec *ind_region_rs(void)
{
	static struct rs_codec *rs;

	if (!rs) {
		rs = rs_init();
		if (!rs) {
			fprintf(stderr, "mkfs.beamfs: cannot initialise the RS codec\n");
			exit(1);
		}
	}
	return rs;
}

/*
 * Decode a region block from @raw into @flat.
 *
 * A fresh region block is all zeroes, which is a valid codeword for
 * all-zero payload, so the decode succeeds and hands back zeroes -- no
 * special case is needed for the first slot written into a block.
 *
 * A block beyond correction aborts the format. During mkfs the only way
 * that happens is a device returning different bytes than were written,
 * and continuing would build a filesystem on top of it.
 */
static void ind_region_decode(uint8_t *raw, uint8_t *flat, uint64_t region_blk)
{
	struct rs_codec *rs = ind_region_rs();
	unsigned int i;

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		uint8_t *sub = raw + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;
		int positions[BEAMFS_RS_PARITY];
		int rc = rs_decode_subblock(rs, sub, BEAMFS_SUBBLOCK_DATA,
					    sub + BEAMFS_SUBBLOCK_DATA,
					    positions);

		if (rc == RS_UNCORRECTABLE) {
			fprintf(stderr, "mkfs.beamfs: parity region block %llu subblock %u is beyond correction; the device is not returning what was written\n",
				(unsigned long long)region_blk, i);
			exit(1);
		}
		memcpy(flat + (size_t)i * BEAMFS_SUBBLOCK_DATA, sub,
		       BEAMFS_SUBBLOCK_DATA);
	}
}

/* Encode @flat back into @raw. */
static void ind_region_encode(const uint8_t *flat, uint8_t *raw)
{
	unsigned int i;

	for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++) {
		uint8_t *sub = raw + (size_t)i * BEAMFS_SUBBLOCK_TOTAL;

		memcpy(sub, flat + (size_t)i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
		encode_rs_userspace(sub, BEAMFS_SUBBLOCK_DATA,
				    sub + BEAMFS_SUBBLOCK_DATA);
	}
}

static void write_ind_parity(int fd, uint64_t blk, const void *buf,
			     uint64_t data_start, uint64_t ind_parity_blk,
			     uint32_t ind_parity_len, uint32_t mode)
{
	uint8_t region[BEAMFS_BLOCK_SIZE];
	uint8_t flat[BEAMFS_DATA_INLINE_BYTES];
	const uint8_t *b = buf;
	uint64_t index, region_blk;
	uint32_t off, within;
	unsigned int slots;
	size_t stride;
	unsigned int i;
	off_t pos;

	if (mode == BEAMFS_IND_PARITY_NONE || ind_parity_blk == 0 ||
	    ind_parity_len == 0 || blk < data_start)
		return;

	if (mode == BEAMFS_IND_PARITY_CRC) {
		stride = BEAMFS_IND_PARITY_CRC_BYTES;
		slots  = BEAMFS_IND_PARITY_CRC_SLOTS;
	} else {
		stride = BEAMFS_IND_PARITY_RS_BYTES;
		slots  = BEAMFS_IND_PARITY_RS_SLOTS;
	}

	index = blk - data_start;
	region_blk = ind_parity_blk + index / slots;
	within = (uint32_t)(index % slots);
	off = within * (uint32_t)stride;

	if (off + stride > BEAMFS_DATA_INLINE_BYTES ||
	    region_blk >= ind_parity_blk + ind_parity_len)
		return;

	/*
	 * Read-modify-write on the payload.
	 *
	 * One region block holds the parity for fourteen indirect blocks
	 * and writing it whole would erase the thirteen neighbours. The
	 * block carries its own FEC, so the edit cannot happen in the raw
	 * bytes: decode, replace the slot, re-encode.
	 *
	 * Indirect blocks are allocated in increasing order but not
	 * written in it -- a double-indirect L1 is allocated before its
	 * L2 children and written after them -- so a scheme that kept one
	 * region block in memory and moved forward would lose slots.
	 */
	pos = (off_t)region_blk * BEAMFS_BLOCK_SIZE;
	if (lseek(fd, pos, SEEK_SET) < 0) {
		perror("lseek");
		exit(1);
	}
	if (read(fd, region, sizeof(region)) != (ssize_t)sizeof(region))
		memset(region, 0, sizeof(region));

	ind_region_decode(region, flat, region_blk);

	if (mode == BEAMFS_IND_PARITY_CRC) {
		uint32_t *slot = (uint32_t *)(flat + off);

		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			slot[i] = crc32(b + (size_t)i * BEAMFS_SUBBLOCK_DATA,
					BEAMFS_SUBBLOCK_DATA);
	} else {
		for (i = 0; i < BEAMFS_DATA_INLINE_SUBBLOCKS; i++)
			encode_rs_userspace(b + (size_t)i * BEAMFS_SUBBLOCK_DATA,
					    BEAMFS_SUBBLOCK_DATA,
					    flat + off + (size_t)i * BEAMFS_RS_PARITY);
	}

	ind_region_encode(flat, region);

	if (lseek(fd, pos, SEEK_SET) < 0) {
		perror("lseek");
		exit(1);
	}
	if (write(fd, region, sizeof(region)) != BEAMFS_BLOCK_SIZE) {
		perror("write");
		exit(1);
	}
}

int main(int argc, char *argv[])
{
	uint64_t inode_table_len = 16; /* default: 16 blocks = 256 inodes */
	uint32_t scheme = BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE;
	const char *scheme_name = "UNIVERSAL_INLINE";
	uint32_t format_version = BEAMFS_VERSION_V5; /* default v5 (sub-1.D onward); -p embedded forces v5 too */
	const char *profile_name = NULL;
	int opt;

	/*
	 * Userspace mirror of kernel BEAMFS_FEATURE_INCOMPAT_* bits.
	 * Bit 8 = PER_INODE_RS (per-inode RS(255,239) parity, decoupled
	 * from s_data_protection_scheme since func-12 sub-2). Activated
	 * via -O per_inode_rs ; OR-ed into sb.s_feat_incompat at write.
	 */
#define MKFS_FEATURE_INCOMPAT_PER_INODE_RS  (1ULL << 8)
#define MKFS_FEATURE_INCOMPAT_INODE_CRC_FULL (1ULL << 16)
#define MKFS_FEATURE_INCOMPAT_IND_PARITY_FEC (1ULL << 17)
#define MKFS_FEATURE_RO_COMPAT_DATA_CSUM    (1ULL << 4)
#define MKFS_FEATURE_RO_COMPAT_DATA_SELFID  (1ULL << 5)
	/*
	 * INODE_CRC_FULL is set on every new volume rather than offered
	 * as an option.
	 *
	 * It is not a feature: without it i_crc32 covers the head of the
	 * inode and none of the ninety-six bytes of block pointers, so a
	 * flipped pointer bit leaves the checksum matching and the
	 * Reed-Solomon correction that would have fixed it is never
	 * entered. A volume written without it is one upset away from
	 * aiming a file at another file's blocks, silently. There is no
	 * case for writing one on purpose.
	 */
	uint64_t feat_incompat = MKFS_FEATURE_INCOMPAT_INODE_CRC_FULL;
	uint64_t ro_compat = 0;
	int data_csum = 0;
	/*
	 * Indirect parity on by default, in its correcting form.
	 *
	 * An indirect block carries up to 512 pointers -- the entire
	 * shape of any file past 45 KiB -- and without this region
	 * nothing protects it. Every other structure has parity: the
	 * superblock, the bitmap, the inode table, the data. The blocks
	 * that say where the data is were the one exception, on a
	 * filesystem that exists to survive single-event upsets.
	 *
	 * RS rather than CRC because CRC detects and cannot correct, and
	 * correcting is the point: a detected-but-uncorrectable pointer
	 * block loses its subtree just as surely as an undetected one,
	 * with a better error message.
	 *
	 * The cost is 256 bytes per data block, a little over six per
	 * cent of the volume. --indirect-parity=none remains for anyone
	 * measuring what the protection costs; it is not a default
	 * anybody should want.
	 */
	int ind_parity_mode = BEAMFS_IND_PARITY_RS;
	int error_budget = 0;

	const char *from_dir = NULL;

	static const struct option long_opts[] = {
		{"profile",  required_argument, NULL, 'p'},
		{"features", required_argument, NULL, 'O'},
		{"from-dir", required_argument, NULL, 'd'},
		{"data-csum", no_argument, NULL, 'C'},
		{"indirect-parity", required_argument, NULL, 'I'},
		{"error-budget", no_argument, NULL, 'B'},
		{"force", no_argument, NULL, 'F'},
		/*
		 * Long-only: there is no short letter left worth spending
		 * on something a campaign asks for and nobody else does.
		 */
		{"poison", no_argument, NULL, 1000},
		/*
		 * Interleave a block's RS symbols, and put the descriptor
		 * inside the coded area. Nine consecutive bytes stop being
		 * fatal and 139 take their place; the header stops being
		 * the one part of a block nothing protects.
		 *
		 * Long-only and off by default: the layout is incompatible
		 * and a kernel that does not know it refuses the mount.
		 */
		{"interleave", no_argument, NULL, 1001},
		{"no-interleave", no_argument, NULL, 1002},
		{NULL, 0, NULL, 0}
	};
	int poison = 0;
	/*
	 * Capsules by default.
	 *
	 * A codeword owning 239 consecutive bytes loses a block to nine
	 * bad ones in a row, which is one ion track through a die.
	 * Interleaved it takes 139, for no extra parity and no
	 * measurable time -- and the descriptor moves inside the coded
	 * area, so the csum and the selfid stop being the one part of a
	 * block nothing protects.
	 *
	 * --no-interleave makes the old layout, for reading a volume an
	 * older kernel wrote.
	 */
	int interleave = 1;

	while ((opt = getopt_long(argc, argv, "N:s:O:d:CI:FBb:", long_opts, NULL)) != -1) {
		switch (opt) {
		case 1000:
			poison = 1;
			break;
		case 1001:
			interleave = 1;
			break;
		case 1002:
			interleave = 0;
			break;
		case 'N':
			{
				uint64_t n = (uint64_t)atoll(optarg);
				uint64_t inodes_per_blk = BEAMFS_BLOCK_SIZE
					/ sizeof(struct beamfs_inode);
				inode_table_len = (n + inodes_per_blk - 1)
					/ inodes_per_blk;
				if (inode_table_len < 1)
					inode_table_len = 1;
			}
			break;
		case 's':
			if (strcmp(optarg, "inline") == 0) {
				scheme = BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE;
				scheme_name = "UNIVERSAL_INLINE";
			} else if (strcmp(optarg, "inode-universal") == 0 ||
				   strcmp(optarg, "inode_universal") == 0) {
				fprintf(stderr,
					"mkfs.beamfs: scheme '%s' is refused: the kernel path for it keeps no parity for indirect blocks (see known-limitations 3.18)\n",
					optarg);
				return 1;
			} else {
				fprintf(stderr,
					"mkfs.beamfs: unknown scheme '%s' (expected: inline)\n",
					optarg);
				return 1;
			}
			break;
		case 'p':
			/*
			 * --profile=<name>: select on-disk format profile.
			 * embedded: v5.0 minimal RFC-able (no INCOMPAT/RO_COMPAT bits).
			 * Future profiles (server, dax) added when their INCOMPAT bits
			 * get _SUPP coverage in beamfs.h.
			 */
			if (strcmp(optarg, "embedded") == 0) {
				format_version = BEAMFS_VERSION_V5;
				profile_name = "embedded";
			} else {
				fprintf(stderr,
					"mkfs.beamfs: unknown profile '%s' (expected: embedded)\n",
					optarg);
				return 1;
			}
			break;
		case 'O':
			/*
			 * -O / --features=<list>: comma-separated INCOMPAT feature
			 * names (ext4-style). Recognised:
			 *   per_inode_rs   bit 8 BEAMFS_FEATURE_INCOMPAT_PER_INODE_RS
			 * The bit must be in BEAMFS_FEAT_INCOMPAT_SUPP on the kernel
			 * side, otherwise mount will fail. As of func-12 sub-2,
			 * SUPP = PER_INODE_RS.
			 */
			{
				char *list = strdup(optarg);
				char *tok, *saveptr = NULL;
				if (!list) {
					fprintf(stderr, "mkfs.beamfs: strdup OOM\n");
					return 1;
				}
				for (tok = strtok_r(list, ",", &saveptr);
				     tok != NULL;
				     tok = strtok_r(NULL, ",", &saveptr)) {
					if (strcmp(tok, "per_inode_rs") == 0) {
						feat_incompat |= MKFS_FEATURE_INCOMPAT_PER_INODE_RS;
					} else {
						fprintf(stderr,
							"mkfs.beamfs: unknown feature '%s' (expected: per_inode_rs)\n",
							tok);
						free(list);
						return 1;
					}
				}
				free(list);
			}
			break;
		case 'B':
			error_budget = 1;
			break;
		case 'b':
			/*
			 * Block size, for callers that pass one.
			 *
			 * The format has exactly one block size and it is not
			 * negotiable: the RS geometry, the subblock layout and
			 * every on-disk offset are derived from 4096. Accepting
			 * the option and rejecting any other value is more
			 * useful than not knowing it at all -- xfstests passes
			 * -b 4096 to every ext2-like filesystem, and mkfs
			 * exiting with "invalid option" made 083 and 015 look
			 * like filesystem failures when they had not started.
			 */
			/*
			 * Refuse anything but 4096, and refuse it before
			 * doing any work.
			 *
			 * The format has exactly one block size: the RS
			 * geometry, the subblock layout and every on-disk
			 * offset derive from it. generic/466 walks 512
			 * through 65536 and expects mkfs to fail on the
			 * sizes a filesystem does not support, then moves
			 * on -- so failing is the correct answer, and the
			 * only wrong one is formatting at 4096 while the
			 * caller believes it asked for something else.
			 */
			if (atoi(optarg) != BEAMFS_BLOCK_SIZE) {
				fprintf(stderr,
					"beamfs: block size %s not supported, only %d\n",
					optarg, BEAMFS_BLOCK_SIZE);
				return 1;
			}
			break;
		case 'F':
			/*
			 * Accepted and ignored. mkfs.beamfs never refuses a
			 * device that already holds a filesystem, so there
			 * is nothing to force; the flag exists because
			 * xfstests passes it unconditionally, as do most
			 * scripts written against mke2fs.
			 */
			break;
		case 'I':
			if (!strcmp(optarg, "none"))
				ind_parity_mode = BEAMFS_IND_PARITY_NONE;
			else if (!strcmp(optarg, "crc"))
				ind_parity_mode = BEAMFS_IND_PARITY_CRC;
			else if (!strcmp(optarg, "rs"))
				ind_parity_mode = BEAMFS_IND_PARITY_RS;
			else {
				fprintf(stderr,
				        "beamfs: --indirect-parity must be none, crc or rs\n");
				return 1;
			}
			break;
		case 'C':
			/*
			 * --data-csum: enable the DATA_CSUM RO_COMPAT feature
			 * (format-v6). Sets RO_COMPAT bit 4 and stamps a CRC32
			 * descriptor into every data block tail pad. Opt-in: absent
			 * this flag the image is byte-identical to v5.
			 */
			ro_compat |= MKFS_FEATURE_RO_COMPAT_DATA_CSUM;
			ro_compat |= MKFS_FEATURE_RO_COMPAT_DATA_SELFID;
			data_csum = 1;
			break;
		case 'd':
			/*
			 * --from-dir <path>: populate the new filesystem with the
			 * contents of <path>. Phase 2 (minimal): supports a single
			 * regular file at <path>/foo, no recursion, no other types.
			 */
			from_dir = optarg;
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-N inodes] [-s scheme] [--profile=embedded] [-O feat,..] [--from-dir=path] <device_or_image>\n"
				"  -b size         block size; only 4096 is supported\n"
				"  -s scheme       inline (default: inline; inode-universal is refused)\n"
				"  --profile=name  embedded (writes v5.0 minimal format)\n"
				"  -O / --features list of INCOMPAT bits (per_inode_rs)\n"
				"  -I / --indirect-parity  rs (default) | crc | none\n"
				"                  rs corrects a flipped pointer, crc only detects one,\n"
				"                  none leaves the block tree unprotected\n"
				"  --from-dir path populate FS with contents of path (Phase 2: 1 regular file)\n"
				"  --data-csum     enable DATA_CSUM + DATA_SELFID (RO_COMPAT bits 4 and 5, format-v6)\n"
				"  --poison        fill the data area with 0xbeef, so a block\n"
				"                  nobody wrote can be told from one somebody did\n"
				"  --interleave    the default: each codeword's symbols spread\n"
				"                  across the block and the descriptor inside the\n"
				"                  coded area -- 139 consecutive bad bytes\n"
				"                  survivable rather than 9\n"
				"  --no-interleave the older layout, for a volume an older\n"
				"                  kernel has to read\n",
				argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr,
			"Usage: %s [-N inodes] [-s scheme] <device_or_image>\n"
			"  -s scheme   inline | inode-universal (default: inline)\n",
			argv[0]);
		return 1;
	}

	int fd = open(argv[optind], O_RDWR);
	if (fd < 0) { perror("open"); return 1; }

	struct stat st;
	if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }

	uint64_t total_bytes;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &total_bytes) < 0) {
			perror("ioctl BLKGETSIZE64");
			return 1;
		}
	} else {
		total_bytes = (uint64_t)st.st_size;
	}

	/*
	 * An explicit block count after the device, mke2fs style.
	 *
	 * _scratch_mkfs_sized() formats a small filesystem on a large
	 * device by passing the count that way, and every test that needs
	 * a sized filesystem goes through it. Without this the argument
	 * was ignored, the whole device was formatted, and tests that
	 * depend on filling a small volume never reached their point.
	 */
	if (optind + 1 < argc) {
		char *endp = NULL;
		unsigned long long want = strtoull(argv[optind + 1], &endp, 10);

		if (!endp || *endp != '\0' || want == 0) {
			fprintf(stderr, "beamfs: bad block count '%s'\n",
				argv[optind + 1]);
			return 1;
		}
		if (want * BEAMFS_BLOCK_SIZE > total_bytes) {
			fprintf(stderr,
				"beamfs: %llu blocks exceeds the device\n", want);
			return 1;
		}
		total_bytes = want * (uint64_t)BEAMFS_BLOCK_SIZE;
	}

	uint64_t total_blocks = total_bytes / BEAMFS_BLOCK_SIZE;
	if (total_blocks < 16) {
		fprintf(stderr, "beamfs: image too small (need >= 16 blocks)\n");
		return 1;
	}

	/* Layout:
	 * Block 0      : superblock
	 * Block 1-4    : inode table (4 blocks = 64 inodes @ 256B each)
	 * Block 5      : bitmap block (RS FEC protected)
	 * Block 6      : root dir data
	 * Block 7      : canary block (INLINE only; absent for other schemes)
	 * Block 8+     : free data blocks (INLINE) / Block 7+ (other schemes)
	 */
	int canary_present = (scheme == BEAMFS_DATA_PROTECTION_UNIVERSAL_INLINE);
	uint64_t inode_table_blk = 1;
	uint64_t bitmap_blk      = inode_table_blk + inode_table_len;
	/*
	 * Multi-block bitmap: K = ceil(s_nblocks / BITS_PER_BITMAP_BLOCK).
	 * BITS_PER_BITMAP_BLOCK = 16 * 239 * 8 = 30592. The data zone is
	 * roughly total_blocks - (1 + inode_table_len + K + reserved); we
	 * compute K with a small fixed-point iteration that converges in a
	 * few rounds for all practical sizes.
	 */
	uint64_t bits_per_bitmap_blk = (uint64_t)BEAMFS_BITMAP_SUBBLOCKS *
	                              BEAMFS_SUBBLOCK_DATA * 8u; /* 30592 */
	uint64_t bitmap_blocks_count = 1;
	uint64_t ind_parity_count = 0;

	/*
	 * Bitmap and parity region are both sized from the data block
	 * count, which is what is left after them -- so both are solved by
	 * the same fixed-point iteration the bitmap already used. Four
	 * rounds converge for every size the format allows.
	 *
	 * The region is sized for every data block being an indirect
	 * block. That is far more than any real filesystem needs, and it
	 * is deliberate: the alternative is a table mapping blocks to
	 * parity slots, which would be metadata in need of protection
	 * itself, and the recursion has to stop somewhere.
	 */
	/*
	 * Slots per region block, not bytes per data block.
	 *
	 * The region block carries its own RS FEC now, so only
	 * BEAMFS_DATA_INLINE_BYTES of its 4096 are payload: fourteen RS
	 * slots fit, or fifty-nine CRC ones, against sixteen and
	 * sixty-four when the block was payload throughout. The region
	 * grows by a seventh, which on a 1 GiB volume is 2341 blocks, and
	 * that is what it costs for the layer protecting the indirect
	 * blocks to be protected itself.
	 */
	uint64_t ind_slots_per_blk =
		(ind_parity_mode == BEAMFS_IND_PARITY_RS)
			? BEAMFS_IND_PARITY_RS_SLOTS :
		(ind_parity_mode == BEAMFS_IND_PARITY_CRC)
			? BEAMFS_IND_PARITY_CRC_SLOTS : 0u;
	/* One byte per data block: the worst subblock's corrected count. */
	uint64_t budget_bytes_per_blk = error_budget ? 1u : 0u;
	uint64_t budget_count = 0;

	for (int iter = 0; iter < 6; iter++) {
		uint64_t fixed_overhead = 1 + inode_table_len + bitmap_blocks_count +
		                         ind_parity_count + budget_count +
		                         (canary_present ? 2 : 1);
		uint64_t data_blocks_est = (total_blocks > fixed_overhead) ?
		                          (total_blocks - fixed_overhead) : 0;
		uint64_t need = (data_blocks_est + bits_per_bitmap_blk - 1) /
		               bits_per_bitmap_blk;
		uint64_t need_par = 0;

		if (need < 1) need = 1;
		if (ind_slots_per_blk) {
			/*
			 * Slots, not bytes. The region block carries its own
			 * FEC, so only BEAMFS_DATA_INLINE_BYTES of its 4096
			 * are payload and the divisor is how many slots fit
			 * in that payload, not how many bytes fit in a block.
			 */
			need_par = (data_blocks_est + ind_slots_per_blk - 1) /
			          ind_slots_per_blk;
		}
		uint64_t need_bud = 0;

		if (budget_bytes_per_blk) {
			need_bud = (data_blocks_est * budget_bytes_per_blk +
				    BEAMFS_BLOCK_SIZE - 1) / BEAMFS_BLOCK_SIZE;
		}
		if (need == bitmap_blocks_count &&
		    need_par == ind_parity_count &&
		    need_bud == budget_count)
			break;
		bitmap_blocks_count = need;
		ind_parity_count = need_par;
		budget_count = need_bud;
	}
	if (bitmap_blocks_count > 0xFFFFu) {
		fprintf(stderr,
		        "beamfs: volume too large for s_flags (need %lu blocks, max 65535)\n",
		        (unsigned long)bitmap_blocks_count);
		return 1;
	}
	uint64_t ind_parity_blk  = bitmap_blk + bitmap_blocks_count;
	uint64_t budget_blk      = ind_parity_blk + ind_parity_count;
	uint64_t after_parity    = budget_blk + budget_count;
	uint64_t root_dir_blk    = after_parity;
	uint64_t canary_blk      = after_parity + 1;
	uint64_t data_start_blk  = canary_present ?
	                          (after_parity + 2) :
	                          (after_parity + 1);
	uint64_t inodes_per_block = BEAMFS_BLOCK_SIZE / sizeof(struct beamfs_inode);
	uint64_t total_inodes    = inode_table_len * inodes_per_block;

	uint8_t zero[BEAMFS_BLOCK_SIZE];
	memset(zero, 0, sizeof(zero));

	/* Zero inode table blocks */
	for (uint64_t i = 0; i < inode_table_len; i++)
		write_block(fd, inode_table_blk + i, zero);

	/*
	 * Zero the parity region.
	 *
	 * write_ind_parity is a read-modify-write, so whatever is on the
	 * device survives into the new filesystem: format a disk that
	 * held a beamfs volume before and its old parity is still there,
	 * describing blocks that now belong to other files. An all-zero
	 * region block is a valid codeword for all-zero payload, so a
	 * fresh one needs no encoding -- but it does need to be fresh.
	 *
	 * generic/310 reformats the test device on a machine that has run
	 * hundreds of tests before it. Blocks 1034 and 1035 came back
	 * with data in fourteen of their sixteen subblocks and no parity
	 * in any of them, and the kernel refused to update the slots they
	 * hold rather than write over damage it could not read.
	 */
	if (ind_parity_count) {
		for (uint64_t i = 0; i < ind_parity_count; i++)
			write_block(fd, ind_parity_blk + i, zero);
	}

	/*
	 * Write K bitmap blocks -- all bits set (= all data blocks free).
	 * 16 sub-blocks of 239 data bytes each, each followed by 16 bytes
	 * of RS(255,239) parity. bitmap_block[] is re-used after this loop
	 * to mark canary_blk allocated (bit < 1912 in all practical sizes,
	 * so the modification lands in block bitmap_blk[0]).
	 */
	/* Phase 4a: multi-block bitmap. Allocate K * BEAMFS_BLOCK_SIZE heap buffer
	 * (was: single 4 KiB stack buffer, only sufficient for ~7.4 MiB data).
	 * Each bitmap block holds BEAMFS_BITMAP_BITS_PER_BLOCK = 30592 bits.
	 * Total capacity: bitmap_blocks_count * 30592 data blocks. */
	size_t bitmap_total_bytes = (size_t)bitmap_blocks_count * BEAMFS_BLOCK_SIZE;
	uint8_t *bitmap_buf = calloc(1, bitmap_total_bytes);
	if (!bitmap_buf) {
		fprintf(stderr, "mkfs.beamfs: OOM allocating bitmap (%zu bytes)\n",
		        bitmap_total_bytes);
		return 1;
	}
	/* Seed all bits to 1 (= all data blocks free) in every bitmap block. */
	for (uint64_t k = 0; k < bitmap_blocks_count; k++) {
		uint8_t *bp_k = bitmap_buf + k * BEAMFS_BLOCK_SIZE;
		for (int s = 0; s < BEAMFS_BITMAP_SUBBLOCKS; s++) {
			memset(bp_k + s * BEAMFS_SUBBLOCK_TOTAL, 0xFF,
			       BEAMFS_SUBBLOCK_DATA);
		}
	}
	/* Note: RS encoding + write-back deferred. The canary bit-mark (if any)
	 * and the --from-dir populate add more marks; final write-back happens
	 * once at the end (or right after canary mark if no --from-dir). */

	/*
	 * Root directory block: variable-length records, then parity.
	 *
	 * The payload is built contiguously in the first 3824 bytes and
	 * scattered into the sixteen codeword slots at the end, which is
	 * what the kernel's beamfs_dirent_encode does and what its walk
	 * expects to find.
	 *
	 * Records were a flat 268 bytes each before DIR_RS, and the block
	 * carried no parity at all -- the one structure on the volume that
	 * did not.
	 */
	uint8_t dir_block[BEAMFS_BLOCK_SIZE];
	uint8_t dir_payload[BEAMFS_DATA_INLINE_BYTES];
	uint32_t dir_off = 0;
	memset(dir_block, 0, sizeof(dir_block));
	memset(dir_payload, 0, sizeof(dir_payload));

	struct beamfs_dir_entry *de =
		(struct beamfs_dir_entry *)dir_payload;
	/* Entry "." */
	de->d_ino      = 1; /* root inode = 1 */
	de->d_name_len = 1;
	de->d_file_type = 4; /* DT_DIR (Linux standard) */
	de->d_name[0]  = '.';
	de->d_rec_len  = BEAMFS_DIRENT_LEN(1);
	dir_off += BEAMFS_DIRENT_LEN(1);

	/* Entry ".." */
	de = (struct beamfs_dir_entry *)(dir_payload + dir_off);
	de->d_ino      = 1;
	de->d_name_len = 2;
	de->d_file_type = 4; /* DT_DIR (Linux standard) */
	de->d_name[0]  = '.';
	de->d_name[1]  = '.';
	de->d_rec_len  = BEAMFS_DIRENT_LEN(2);
	dir_off += BEAMFS_DIRENT_LEN(2);

	/*
	 * No directory entry for the canary.
	 *
	 * format-v4.md sec 11.1 is explicit: "In v4, the fixture exists
	 * on-disk only; no VFS alias is exposed". v5 and v6 describe it
	 * the same way -- a block at bitmap_blk + 2, nothing more. The
	 * entry written here was ahead of the format and cost more than
	 * it gave.
	 *
	 * The kernel marks every inode below BEAMFS_FIRST_USER_INO
	 * immutable, so an exposed canary is a file root cannot delete:
	 * rm -rf on any beamfs volume fails, every test that cleans up
	 * that way leaves its directories behind, and generic/589 fails
	 * on a mount tree polluted by what the previous case could not
	 * remove. It also showed a 1970 timestamp on freshly formatted
	 * volumes, which reads as a broken volume to anyone looking.
	 *
	 * Inode 2 still exists and still points at canary_blk, so fsck
	 * walks the fixture and validates the RS chain end to end. Only
	 * the readdir entry is gone, which is what the format says.
	 */

	/*
	 * One free record covering everything left, so the kernel's walk
	 * has a length to step by rather than stopping at a run of zeros.
	 */
	de = (struct beamfs_dir_entry *)(dir_payload + dir_off);
	de->d_rec_len = (uint16_t)(BEAMFS_DATA_INLINE_BYTES - dir_off);

	/* Scatter into the codeword slots, then encode. */
	for (unsigned int sb_i = 0; sb_i < BEAMFS_DATA_INLINE_SUBBLOCKS; sb_i++)
		memcpy(dir_block + (size_t)sb_i * BEAMFS_SUBBLOCK_TOTAL,
		       dir_payload + (size_t)sb_i * BEAMFS_SUBBLOCK_DATA,
		       BEAMFS_SUBBLOCK_DATA);
	rs_encode_bitmap(dir_block);

	write_block(fd, root_dir_blk, dir_block);

	/*
	 * Conformance fixture (canary block) -- INLINE scheme only.
	 * See Documentation/format-v4.md section 13.
	 *
	 * Layout: 64-byte ASCII header + 3760-byte deterministic payload
	 * (XOR shift fingerprint), encoded as 16 RS(255,239) interleaved
	 * subblocks via rs_encode_bitmap (same on-disk layout). Block byte
	 * image is reproducible across mkfs invocations; SHA256 is
	 * published in format-v4.md section 13.
	 */
	if (canary_present) {
		uint8_t canary_block[BEAMFS_BLOCK_SIZE];
		uint8_t canary_user[BEAMFS_CANARY_USER_BYTES];
		const char *hdr = BEAMFS_CANARY_HEADER_STR;
		size_t hdr_len = strlen(hdr);
		size_t i;

		if (hdr_len >= BEAMFS_CANARY_HEADER_LEN) {
			fprintf(stderr, "mkfs.beamfs: canary header too long (%zu >= %d)\n",
				hdr_len, BEAMFS_CANARY_HEADER_LEN);
			return 1;
		}

		/* Header: ASCII string + zero pad to BEAMFS_CANARY_HEADER_LEN */
		memset(canary_user, 0, BEAMFS_CANARY_HEADER_LEN);
		memcpy(canary_user, hdr, hdr_len);

		/* Payload: deterministic XOR shift fingerprint */
		for (i = 0; i < BEAMFS_CANARY_PAYLOAD_LEN; i++) {
			uint32_t x = (uint32_t)i;
			canary_user[BEAMFS_CANARY_HEADER_LEN + i] =
				(uint8_t)((x ^ (x >> 8)) & 0xFFu);
		}

		memset(canary_block, 0, sizeof(canary_block));
		if (interleave) {
			/*
			 * A capsule: the user bytes go down contiguously
			 * and the encoder gathers each codeword's symbols
			 * from every sixteenth byte. The descriptor that
			 * stamp_data_csum writes at 3808 is inside the
			 * coded area, so it is sealed with the rest --
			 * which is the whole point, and why the stamp has
			 * to come before the encode here and after it in
			 * the alternating layout.
			 */
			memcpy(canary_block, canary_user,
			       BEAMFS_CAPSULE_DATA_BYTES);
		} else {
			/* 16 subblocks of 239 user bytes, parity after each */
			for (int s = 0; s < BEAMFS_DATA_INLINE_SUBBLOCKS; s++) {
				memcpy(canary_block + s * BEAMFS_SUBBLOCK_TOTAL,
				       canary_user + s * BEAMFS_SUBBLOCK_DATA,
				       BEAMFS_SUBBLOCK_DATA);
			}
			rs_encode_bitmap(canary_block);
		}
		if (interleave) {
			stamp_capsule_csum(canary_block, canary_user,
					   data_csum,
					   BEAMFS_RESERVED_INO_CANARY, 0);
			rs_encode_capsule(canary_block);
		} else {
			stamp_data_csum(canary_block, canary_user, data_csum,
					BEAMFS_RESERVED_INO_CANARY, 0);
		}
		write_block(fd, canary_blk, canary_block);

		/*
		 * The canary block is NOT marked in the bitmap.
		 *
		 * It sits below data_start, alongside the superblock, the
		 * inode table, the bitmap itself and the root directory --
		 * reserved space the allocator never covers. alloc.c says so
		 * outright: "canary blocks are not supposed to be
		 * bitmap-covered".
		 *
		 * mkfs marked it anyway, and with an index computed from the
		 * absolute block number rather than one relative to
		 * data_start, so it set a bit belonging to an unrelated data
		 * block. Every freshly formatted volume therefore carried one
		 * block marked used that no inode referenced -- reported by
		 * fsck pass 4, and lost to the allocator for the life of the
		 * filesystem.
		 *
		 * The stale comment claiming canary_blk < 1912 was already
		 * untrue before the indirection parity region moved
		 * data_start further out.
		 */
	}

	/* Write root inode (inode 1) */
	uint8_t inode_block[BEAMFS_BLOCK_SIZE];
	memset(inode_block, 0, sizeof(inode_block));

	struct beamfs_inode *ri = (struct beamfs_inode *)inode_block;
	/* inode 1 = first slot in block 1 */
	ri->i_mode   = 0040755; /* drwxr-xr-x */
	ri->i_uid    = 0;
	ri->i_gid    = 0;
	ri->i_nlink  = 2;
	/* Payload per block, not block size: 3824 bytes of records
	 * and 272 of parity. i_size counting the parity makes a walk
	 * read it as a directory record. */
	ri->i_size   = BEAMFS_DATA_INLINE_BYTES;
	ri->i_direct[0] = root_dir_blk;
	ri->i_crc32  = crc32_inode(ri);

	/*
	 * Compute RS parity over the first BEAMFS_INODE_RS_DATA bytes of
	 * the root inode. The parity lands in i_reserved[0..15]. The rest
	 * of i_reserved[16..83] is already zero from the memset above.
	 */
	rs_encode_inode((uint8_t *)ri);

	/*
	 * Inode 2 -- canary regular file alias (INLINE only).
	 * Lives in slot 2 of the first inode-table block (offset
	 * sizeof(struct beamfs_inode) within inode_block). Points
	 * to canary_blk via i_direct[0]. Timestamps are epoch 0
	 * for byte-deterministic reproducibility of the inode block
	 * across mkfs invocations.
	 * Immutable flag deferred to v4.1 (Palier 2.5 full).
	 */
	if (canary_present) {
		struct beamfs_inode *ci = (struct beamfs_inode *)
		    (inode_block + sizeof(struct beamfs_inode));
		ci->i_mode      = 0100644; /* regular, rw-r--r-- */
		ci->i_nlink     = 1;
		ci->i_uid       = 0;
		ci->i_gid       = 0;
		/*
		 * What one block of this volume holds: 3808 in a capsule,
		 * where the last sixteen of the coded area are the
		 * descriptor, and 3824 otherwise. Writing 3824 on a
		 * capsule volume has fsck report the canary as needing
		 * two blocks when it owns one.
		 */
		ci->i_size      = interleave ? BEAMFS_CAPSULE_DATA_BYTES
					     : BEAMFS_DATA_INLINE_BYTES;
		ci->i_atime     = 0; /* epoch 0 -- deterministic */
		ci->i_mtime     = 0;
		ci->i_ctime     = 0;
		ci->i_flags     = 0;
		ci->i_direct[0] = canary_blk;
		ci->i_crc32     = crc32_inode(ci);
		rs_encode_inode((uint8_t *)ci);
	}

	write_block(fd, inode_table_blk, inode_block);

	/* Write superblock */
	struct beamfs_super_block sb;
	memset(&sb, 0, sizeof(sb));
	sb.s_magic          = BEAMFS_MAGIC;
	sb.s_block_size     = BEAMFS_BLOCK_SIZE;
	sb.s_block_count    = total_blocks;
	/*
	 * s_free_blocks counts data blocks not (yet) allocated.
	 *
	 * data_start_blk already accounts for everything below it: the
	 * inode table, the bitmap, the indirect parity region, the budget
	 * blocks, the root directory and, under INLINE, the canary. The
	 * arithmetic is a few lines above and the layout it produces is
	 * what s_data_start_blk records; nothing here needs to repeat it,
	 * and the old note that put the canary at bitmap_blk+2 stopped
	 * being true when the parity region grew between them.
	 */
	sb.s_free_blocks    = total_blocks - data_start_blk;
	sb.s_inode_count    = total_inodes;
	/*
	 * s_free_inodes: root (inode 1) always used. Under INLINE,
	 * inode 2 (canary alias) is also pre-allocated by mkfs.
	 */
	sb.s_free_inodes    = total_inodes - (canary_present ? 2 : 1);
	sb.s_inode_table_blk = inode_table_blk;
	/*
	 * The pattern goes down before the superblock declares the
	 * volume: a reader that finds it after this point is reading a
	 * block nothing has written since the format.
	 */
	if (poison &&
	    poison_data_area(fd, data_start_blk,
			     total_blocks - data_start_blk) < 0)
		return 1;

	sb.s_data_start_blk  = data_start_blk;
	sb.s_version        = format_version;
	sb.s_bitmap_blk     = bitmap_blk;
	sb.s_ind_parity_blk  = ind_parity_count ? ind_parity_blk : 0;
	sb.s_ind_parity_len  = (uint32_t)ind_parity_count;
	sb.s_ind_parity_mode = (uint32_t)ind_parity_mode;
	/*
	 * Zero the region before declaring it. mkfs allocated it and left
	 * whatever the medium held, so a fresh volume reported 9.2
	 * million blocks at maximum wear -- residue from the last thing
	 * on the stick, read as a wear record. A gauge that lies about a
	 * new volume is worse than no gauge.
	 */
	if (budget_count) {
		uint8_t zblk[BEAMFS_BLOCK_SIZE];
		uint64_t b;

		memset(zblk, 0, sizeof(zblk));
		for (b = 0; b < budget_count; b++)
			write_block(fd, budget_blk + b, zblk);
	}

	sb.s_budget_blk      = budget_count ? budget_blk : 0;
	sb.s_budget_len      = (uint32_t)budget_count;
	sb.s_budget_pad      = 0;
	if (error_budget)
		feat_incompat |= BEAMFS_FEATURE_INCOMPAT_ERROR_BUDGET;

	/*
	 * Directory parity is not optional. It closes the last unprotected
	 * structure on the volume, and a volume formatted without it would
	 * be one the kernel has to keep a second directory format for.
	 */
	feat_incompat |= BEAMFS_FEATURE_INCOMPAT_DIR_RS;

	/*
	 * The parity region carries its own FEC, which changes how many
	 * slots a region block holds -- fourteen under RS instead of
	 * sixteen. A kernel reading an older volume with this arithmetic
	 * would address the wrong slots and hand back another block's
	 * parity, so the flag is what stops it.
	 *
	 * Only when a region exists: with parity off there is nothing to
	 * protect and nothing to be incompatible about.
	 */
	if (ind_parity_mode != BEAMFS_IND_PARITY_NONE)
		feat_incompat |= MKFS_FEATURE_INCOMPAT_IND_PARITY_FEC;
	if (ind_parity_mode != BEAMFS_IND_PARITY_NONE)
		feat_incompat |= BEAMFS_FEATURE_INCOMPAT_INDIRECT_PARITY;
	sb.s_feat_compat    = 0;
	if (interleave)
		feat_incompat |= BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE;

	sb.s_feat_incompat  = feat_incompat;
	sb.s_feat_ro_compat = ro_compat;
	sb.s_data_protection_scheme = scheme;
	/*
	 * s_flags encoding (CRC32 region A):
	 *   bits 0..15 = bitmap_blocks_count (1 -> legacy single-block)
	 *   bits 16..31 = reserved (must be zero)
	 */
	sb.s_flags = (uint32_t)(bitmap_blocks_count & 0xFFFFu);

	/*
	 * --from-dir Phase 3e : recursive populate of a directory tree.
	 *
	 * Iterative DFS walk (explicit stack to avoid C recursion depth
	 * concerns on deep trees). For each entry encountered:
	 *   REG : allocate inode + 1 data block (limited to BEAMFS_DATA_INLINE_BYTES
	 *         = 3824 bytes per file; Phase 3d adds indirect cascade)
	 *   DIR : allocate inode + 1 dirblock (limited to 15 entries per dir
	 *         due to single-dirblock layout; Phase 3e adds multi-block dirs)
	 *   LNK : fast symlink (target inline in i_direct[], <= 96 bytes)
	 *   other: skipped with warning
	 *
	 * Multi-block inode table: supports up to inode_table_len * 16 inodes
	 * (16 inodes per block). Inode block index = (ino - 1) / 16.
	 *
	 * Limits Phase 3c:
	 *   - Files: <= 3824 bytes each
	 *   - Dirs : <= 15 children each (incl. . .. but excluding canary
	 *            which only lives in root)
	 *   - Data block index < 8*239 = 1912 (bitmap stays in block 0)
	 *     => max ~7.4 MiB of data (~1900 files of 3824 B). Phase 4
	 *     extends to multi-block bitmap modifications.
	 *
	 * R12: each on-disk write goes through write_block() with explicit
	 * RS encode where applicable (data blocks via rs_encode_bitmap;
	 * inodes via rs_encode_inode; bitmap via rs_encode_bitmap; inode
	 * blocks themselves are NOT block-level RS encoded under scheme=2,
	 * only the inode's i_reserved[0..15] carry per-inode RS parity).
	 */
	if (from_dir) {
		/* Allocate a buffer for the full inode table (inode_table_len blocks).
		 * The mkfs flow writes inode_table_blk first (one block) before our
		 * code runs; we mirror that block 0 here, then add new inodes either
		 * in block 0 or in blocks 1..inode_table_len-1.
		 *
		 * Conservative memory cap: 16 inode table blocks * 4 KiB = 64 KiB,
		 * sufficient for 256 inodes. With -N up to 32768 inodes the table is
		 * 2048 blocks = 8 MiB, also fine on a build host.
		 */
		size_t it_bytes = (size_t)inode_table_len * BEAMFS_BLOCK_SIZE;
		uint8_t *it_buf = calloc(1, it_bytes);
		if (!it_buf) {
			fprintf(stderr, "mkfs.beamfs --from-dir: OOM allocating inode table (%zu bytes)\n", it_bytes);
			return 1;
		}
		/* Block 0 already written above (with root + canary). Copy from
		 * the in-memory inode_block (which still holds that state). */
		memcpy(it_buf, inode_block, BEAMFS_BLOCK_SIZE);

		/* Pre-allocated subdir count for root inode (will be added to its
		 * i_nlink at end). */
		unsigned root_subdir_count = 0;

		/* Allocators */
		uint64_t next_ino = canary_present ? 3 : 2;
		uint64_t next_blk = data_start_blk;
		/*
		 * Byte offset of the next free record, not a slot index.
		 * Records are variable length under DIR_RS, so counting
		 * slots no longer says where anything is. dir_off is what
		 * the root block above left behind.
		 */
		unsigned root_dirent_slot = dir_off;

		/* The outer one, computed the same way, is in scope here. */

		/* Stack frame for DFS walk. Each frame represents a directory
		 * being populated.
		 *   - dir_path  : host path
		 *   - dir_ino   : inode number to write
		 *   - dir_block : 4 KiB buffer for the dirblock
		 *   - dir_blk   : data block number where the dirblock will be written
		 *   - dirent_slot: next free dirent slot in dir_block
		 *   - subdir_count: number of subdirs in this dir (for nlink)
		 *   - parent_ino: for non-root frames, used for '..' entry
		 *   - is_root  : true for the root frame (writes into existing
		 *                dir_block + uses canary slot, doesn't allocate new
		 *                dirblock since root_dir_blk is pre-allocated)
		 */
		struct frame {
			char *path;
			uint64_t dir_ino;
			uint64_t parent_ino;
			/* Phase 4c: multi-block dirs.
			 * Up to BEAMFS_DIRECT_BLOCKS=12 direct dirblocks + indirect (512 ptrs)
			 * = max 524 dirblocks per dir = max 7860 entries.
			 * Each dirblock holds 15 dirents (BEAMFS_BLOCK_SIZE / 268).
			 * - n_dir_blocks    : number of dirblocks used (>= 1)
			 * - dir_blk_nums[i] : on-disk block number for the i-th dirblock
			 * - dir_blocks_buf  : heap buffer holding n_dir_blocks * BLOCK_SIZE
			 * - dir_indirect_blk: indirect block for dirblocks > 12 (0 if unused)
			 * - dir_indirect_buf: __le64[512] indirect block contents (heap)
			 */
			unsigned n_dir_blocks;
			uint64_t *dir_blk_nums;
			uint8_t  *dir_blocks_buf;
			uint64_t  dir_indirect_blk;
			uint8_t  *dir_indirect_buf;
			unsigned dirent_slot;  /* byte offset within last dirblock */
			unsigned subdir_count;
			int is_root;
			/* Iteration state: names of children, sorted, with index */
			char **names;
			int    n_names;
			int    name_idx;
		};

		/* Helper macro: pointer to current dirblock buffer (last allocated). */
		#define FRAME_CUR_DIRBLOCK(f) \
			((f)->dir_blocks_buf + ((f)->n_dir_blocks - 1) * BEAMFS_BLOCK_SIZE)
		/*
		 * Records are variable length, so a block is full when the
		 * next one will not fit rather than after a fixed count.
		 */
		#define BEAMFS_DIRENT_ROOM(off, len) \
			((off) + (len) <= BEAMFS_DATA_INLINE_BYTES)

		/* Cap depth to a sane value for a rootfs. Yocto rootfs typically
		 * has depth <= 10. */
		#define MAX_DFS_DEPTH 32
		struct frame stack[MAX_DFS_DEPTH];
		int sp = 0; /* stack pointer */

		/* Initialise root frame (Phase 4c: single dirblock initially;
		 * extended on demand if > 15 dirents needed). */
		stack[0].path = strdup(from_dir);
		stack[0].dir_ino = 1; /* root inode */
		stack[0].parent_ino = 1; /* root's parent is itself */
		stack[0].n_dir_blocks = 1;
		stack[0].dir_blk_nums = calloc(BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS, sizeof(uint64_t));
		stack[0].dir_blocks_buf = calloc(1, BEAMFS_BLOCK_SIZE);
		stack[0].dir_indirect_blk = 0;
		stack[0].dir_indirect_buf = NULL;
		if (!stack[0].dir_blk_nums || !stack[0].dir_blocks_buf) {
			fprintf(stderr, "mkfs.beamfs --from-dir: OOM root frame\n");
			return 1;
		}
		stack[0].dir_blk_nums[0] = root_dir_blk;
		memcpy(stack[0].dir_blocks_buf, dir_block, BEAMFS_BLOCK_SIZE);
		stack[0].dirent_slot = root_dirent_slot;
		stack[0].subdir_count = 0;
		stack[0].is_root = 1;
		stack[0].names = NULL;
		stack[0].n_names = 0;
		stack[0].name_idx = 0;
		sp = 1;

		uint64_t files_written = 0;
		uint64_t dirs_written  = 0;
		uint64_t hardlinks_dedup = 0;

		/* Phase 4d: hardlink dedup table.
		 * Maps (source dev_t, source ino_t) -> beamfs inode number, so that
		 * multiple host-side hardlinks of the same file map to a single
		 * beamfs inode (with i_nlink incremented).
		 *
		 * Linear array search. Yocto rootfs has ~15 unique inodes with
		 * hardlinks (177 total hardlinked occurrences), so the table stays
		 * tiny and linear search is O(15) per lookup -- negligible.
		 */
		struct htab_entry {
			dev_t dev;
			ino_t ino;
			uint64_t beamfs_ino;
		};
		#define MAX_HTAB_ENTRIES 4096
		struct htab_entry *htab = calloc(MAX_HTAB_ENTRIES, sizeof(struct htab_entry));
		unsigned htab_n = 0;
		if (!htab) {
			fprintf(stderr, "mkfs.beamfs --from-dir: OOM htab alloc\n");
			free(it_buf);
			return 1;
		}

		/* Main DFS loop. We process the top frame; if it has unread
		 * children, push them; else pop. */
		while (sp > 0) {
			struct frame *cur = &stack[sp - 1];

			/* First time we see this frame: scan its directory */
			if (cur->names == NULL && cur->name_idx == 0) {
				DIR *d = opendir(cur->path);
				if (!d) {
					fprintf(stderr, "mkfs.beamfs --from-dir: opendir %s: %s\n",
						cur->path, strerror(errno));
					free(it_buf);
					return 1;
				}
				/* Conservative cap for names per dir at this phase */
				#define MAX_NAMES_PER_DIR 4096
				cur->names = calloc(MAX_NAMES_PER_DIR, sizeof(char *));
				if (!cur->names) {
					closedir(d);
					fprintf(stderr, "mkfs.beamfs --from-dir: OOM names alloc\n");
					free(it_buf);
					return 1;
				}
				struct dirent *de_src;
				while ((de_src = readdir(d)) != NULL) {
					if (strcmp(de_src->d_name, ".") == 0 ||
					    strcmp(de_src->d_name, "..") == 0)
						continue;
					if (cur->n_names >= MAX_NAMES_PER_DIR) {
						fprintf(stderr,
							"mkfs.beamfs --from-dir: too many entries in %s\n",
							cur->path);
						closedir(d);
						free(it_buf);
						return 1;
					}
					cur->names[cur->n_names++] = strdup(de_src->d_name);
				}
				closedir(d);
				/* Sort alphabetically */
				for (int i = 0; i < cur->n_names - 1; i++) {
					for (int j = i + 1; j < cur->n_names; j++) {
						if (strcmp(cur->names[i], cur->names[j]) > 0) {
							char *t = cur->names[i];
							cur->names[i] = cur->names[j];
							cur->names[j] = t;
						}
					}
				}
				cur->name_idx = 0;
			}

			/* If we've processed all children, finalize and pop */
			if (cur->name_idx >= cur->n_names) {
				/*
				 * Phase 4c: scatter each dirblock into its
				 * codeword slots, encode, and write.
				 *
				 * The frame builds records contiguously in the
				 * first 3824 bytes, which is what the kernel's
				 * walk expects to find after decoding. Without
				 * this the block goes out with payload where
				 * the parity belongs and no parity at all --
				 * the kernel reads it, RS says uncorrectable,
				 * and every directory in the image comes back
				 * EUCLEAN.
				 */
				for (unsigned i = 0; i < cur->n_dir_blocks; i++) {
					uint8_t *src = cur->dir_blocks_buf +
						       (size_t)i * BEAMFS_BLOCK_SIZE;
					uint8_t enc[BEAMFS_BLOCK_SIZE];
					unsigned sb_i;

					memset(enc, 0, sizeof(enc));
					/* Trailing free record, so the walk has
					 * a length to step by rather than a run
					 * of zeros. */
					{
						struct beamfs_dir_entry *tail =
						  (struct beamfs_dir_entry *)
						  (src + (i == cur->n_dir_blocks - 1 ?
							  cur->dirent_slot : 0));
						if (i == cur->n_dir_blocks - 1 &&
						    cur->dirent_slot <
						    BEAMFS_DATA_INLINE_BYTES)
							tail->d_rec_len = (uint16_t)
							 (BEAMFS_DATA_INLINE_BYTES -
							  cur->dirent_slot);
					}
					for (sb_i = 0;
					     sb_i < BEAMFS_DATA_INLINE_SUBBLOCKS;
					     sb_i++)
						memcpy(enc + (size_t)sb_i *
							     BEAMFS_SUBBLOCK_TOTAL,
						       src + (size_t)sb_i *
							     BEAMFS_SUBBLOCK_DATA,
						       BEAMFS_SUBBLOCK_DATA);
					rs_encode_bitmap(enc);
					write_block(fd, cur->dir_blk_nums[i], enc);
				}
				/* Phase 4c: write indirect block if used */
				if (cur->dir_indirect_blk) {
					write_block(fd, cur->dir_indirect_blk, cur->dir_indirect_buf);
					write_ind_parity(fd, cur->dir_indirect_blk, cur->dir_indirect_buf,
							 data_start_blk, ind_parity_blk,
							 (uint32_t)ind_parity_count, ind_parity_mode);
				}

				/* Update this dir's inode (i_nlink, i_size, i_direct, i_indirect)
				 * and write to it_buf */
				uint64_t slot = (cur->dir_ino - 1);
				uint64_t blk_idx = slot / inodes_per_block;
				uint64_t blk_off = slot % inodes_per_block;
				struct beamfs_inode *di = (struct beamfs_inode *)
				    (it_buf + blk_idx * BEAMFS_BLOCK_SIZE
				            + blk_off * sizeof(struct beamfs_inode));
				di->i_nlink = (uint16_t)(2 + cur->subdir_count);
				di->i_size  = (uint64_t)cur->n_dir_blocks * BEAMFS_DATA_INLINE_BYTES;
				/* Populate i_direct[] from dir_blk_nums (first 12) */
				for (unsigned i = 0; i < cur->n_dir_blocks && i < BEAMFS_DIRECT_BLOCKS; i++) {
					di->i_direct[i] = cur->dir_blk_nums[i];
				}
				if (cur->dir_indirect_blk) {
					di->i_indirect = cur->dir_indirect_blk;
				}
				di->i_crc32 = crc32_inode(di);
				rs_encode_inode((uint8_t *)di);

				/* If non-root, propagate subdir count up the stack */
				if (!cur->is_root && sp >= 2) {
					stack[sp - 2].subdir_count++;
				}

				/* Cleanup names + dirblock buffers */
				for (int i = 0; i < cur->n_names; i++)
					free(cur->names[i]);
				free(cur->names);
				free(cur->path);
				free(cur->dir_blocks_buf);
				free(cur->dir_blk_nums);
				free(cur->dir_indirect_buf);
				sp--;
				continue;
			}

			/* Process next child */
			const char *name = cur->names[cur->name_idx];
			char child_path[4096];
			int rc = snprintf(child_path, sizeof(child_path), "%s/%s",
			                  cur->path, name);
			if (rc < 0 || (size_t)rc >= sizeof(child_path)) {
				fprintf(stderr, "mkfs.beamfs --from-dir: path too long for %s\n", name);
				free(it_buf);
				return 1;
			}
			cur->name_idx++;

			struct stat cst;
			if (lstat(child_path, &cst) < 0) {
				fprintf(stderr, "mkfs.beamfs --from-dir: lstat %s: %s\n",
					child_path, strerror(errno));
				free(it_buf);
				return 1;
			}

			/* Filename length check */
			if (strlen(name) > BEAMFS_MAX_FILENAME) {
				fprintf(stderr, "mkfs.beamfs --from-dir: skipping %s (name too long)\n",
					child_path);
				continue;
			}

			/* Phase 3e: REG, DIR, and fast LNK (target <= 96 bytes) supported;
			 * other types (sockets, fifos, devices) skipped with warning. */
			if (!S_ISREG(cst.st_mode) && !S_ISDIR(cst.st_mode) && !S_ISLNK(cst.st_mode)) {
				fprintf(stderr, "mkfs.beamfs --from-dir: skipping %s (type 0%o not supported)\n",
					child_path, cst.st_mode & S_IFMT);
				continue;
			}

			/* Phase 4c: dirent slot bounds check.
			 * If current dirblock is full (15 dirents), allocate a new one.
			 * Capacity: up to 12 direct + 512 indirect = 524 dirblocks = 7860 entries.
			 */
			if (!BEAMFS_DIRENT_ROOM(cur->dirent_slot,
						BEAMFS_DIRENT_LEN(strlen(name)))) {
				if (cur->n_dir_blocks >= BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS) {
					fprintf(stderr, "mkfs.beamfs --from-dir: skipping %s (dir at max capacity %u dirblocks)\n",
						child_path,
						(unsigned)(BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS));
					continue;
				}

				/* Need to allocate a new dirblock. Grow heap buffer first. */
				uint8_t *new_buf = realloc(cur->dir_blocks_buf,
				                           (size_t)(cur->n_dir_blocks + 1) * BEAMFS_BLOCK_SIZE);
				if (!new_buf) {
					fprintf(stderr, "mkfs.beamfs --from-dir: OOM dir_blocks_buf grow\n");
					free(it_buf);
					return 1;
				}
				cur->dir_blocks_buf = new_buf;
				memset(new_buf + cur->n_dir_blocks * BEAMFS_BLOCK_SIZE, 0, BEAMFS_BLOCK_SIZE);

				/* Allocate on-disk block for the new dirblock */
				uint64_t new_dirblk = next_blk++;
				if (new_dirblk >= (uint64_t)(bitmap_blocks_count * BEAMFS_BITMAP_BITS_PER_BLOCK)) {
					fprintf(stderr, "mkfs.beamfs --from-dir: new dirblock %lu beyond bitmap range\n",
						(unsigned long)new_dirblk);
					free(it_buf);
					return 1;
				}
				/* Mark bitmap for new dirblock */
				{
					uint64_t bit = new_dirblk - data_start_blk;
					uint64_t bmap_idx = bit / BEAMFS_BITMAP_BITS_PER_BLOCK;
					uint64_t bit_in_bmap = bit % BEAMFS_BITMAP_BITS_PER_BLOCK;
					uint64_t sub = bit_in_bmap / (BEAMFS_SUBBLOCK_DATA * 8);
					uint64_t bit_in_sub = bit_in_bmap % (BEAMFS_SUBBLOCK_DATA * 8);
					uint64_t byte_in_sub = bit_in_sub / 8;
					unsigned bit_in_byte = (unsigned)(bit_in_sub % 8);
					uint8_t *bp = bitmap_buf
					              + bmap_idx * BEAMFS_BLOCK_SIZE
					              + sub * BEAMFS_SUBBLOCK_TOTAL
					              + byte_in_sub;
					*bp &= (uint8_t)~(1u << bit_in_byte);
				}

				/* Record block number. If beyond direct[12], also fill indirect. */
				cur->dir_blk_nums[cur->n_dir_blocks] = new_dirblk;
				if (cur->n_dir_blocks >= BEAMFS_DIRECT_BLOCKS) {
					/* Need indirect block; allocate if not yet */
					if (!cur->dir_indirect_blk) {
						cur->dir_indirect_blk = next_blk++;
						if (cur->dir_indirect_blk >= (uint64_t)(bitmap_blocks_count * BEAMFS_BITMAP_BITS_PER_BLOCK)) {
							fprintf(stderr, "mkfs.beamfs --from-dir: dir indirect block %lu beyond bitmap\n",
								(unsigned long)cur->dir_indirect_blk);
							free(it_buf);
							return 1;
						}
						cur->dir_indirect_buf = calloc(1, BEAMFS_BLOCK_SIZE);
						if (!cur->dir_indirect_buf) {
							fprintf(stderr, "mkfs.beamfs --from-dir: OOM dir indirect buf\n");
							free(it_buf);
							return 1;
						}
						/* Mark bitmap for indirect block */
						{
							uint64_t bit = cur->dir_indirect_blk - data_start_blk;
							uint64_t bmap_idx = bit / BEAMFS_BITMAP_BITS_PER_BLOCK;
							uint64_t bit_in_bmap = bit % BEAMFS_BITMAP_BITS_PER_BLOCK;
							uint64_t sub = bit_in_bmap / (BEAMFS_SUBBLOCK_DATA * 8);
							uint64_t bit_in_sub = bit_in_bmap % (BEAMFS_SUBBLOCK_DATA * 8);
							uint64_t byte_in_sub = bit_in_sub / 8;
							unsigned bit_in_byte = (unsigned)(bit_in_sub % 8);
							uint8_t *bp = bitmap_buf
							              + bmap_idx * BEAMFS_BLOCK_SIZE
							              + sub * BEAMFS_SUBBLOCK_TOTAL
							              + byte_in_sub;
							*bp &= (uint8_t)~(1u << bit_in_byte);
						}
					}
					/* Record dirblock pointer in indirect at slot (n_dir_blocks - DIRECT_BLOCKS) */
					uint64_t ind_slot = cur->n_dir_blocks - BEAMFS_DIRECT_BLOCKS;
					((uint64_t *)cur->dir_indirect_buf)[ind_slot] = new_dirblk;
				}

				cur->n_dir_blocks++;
				cur->dirent_slot = 0; /* reset slot index for new dirblock */
			}

			/* Inode allocation bounds check */
			if (next_ino > inode_table_len * inodes_per_block) {
				fprintf(stderr, "mkfs.beamfs --from-dir: out of inodes (max %lu)\n",
					(unsigned long)(inode_table_len * inodes_per_block));
				free(it_buf);
				return 1;
			}

			uint64_t child_ino = next_ino++;
			uint64_t child_slot = child_ino - 1;
			uint64_t child_blk_idx = child_slot / inodes_per_block;
			uint64_t child_blk_off = child_slot % inodes_per_block;
			struct beamfs_inode *ci = (struct beamfs_inode *)
			    (it_buf + child_blk_idx * BEAMFS_BLOCK_SIZE
			            + child_blk_off * sizeof(struct beamfs_inode));

			/* Add dirent in current dirblock */
			size_t nlen = strlen(name);
			uint16_t rlen = (uint16_t)BEAMFS_DIRENT_LEN(nlen);
			struct beamfs_dir_entry *cde = (struct beamfs_dir_entry *)
			    (FRAME_CUR_DIRBLOCK(cur) + cur->dirent_slot);
			cde->d_ino       = child_ino;
			cde->d_name_len  = (uint8_t)nlen;
			memcpy(cde->d_name, name, nlen);
			cde->d_rec_len   = rlen;
			cur->dirent_slot += rlen;

			if (S_ISREG(cst.st_mode)) {
				cde->d_file_type = 8; /* DT_REG */

				/* Phase 4d: hardlink dedup. If this (dev, ino) is already in htab,
				 * point the dirent at the existing beamfs inode and bump its i_nlink.
				 * Skip alloc + data block writes entirely. */
				if (cst.st_nlink > 1) {
					uint64_t existing_ino = 0;
					for (unsigned h = 0; h < htab_n; h++) {
						if (htab[h].dev == cst.st_dev && htab[h].ino == cst.st_ino) {
							existing_ino = htab[h].beamfs_ino;
							break;
						}
					}
					if (existing_ino) {
						/* Dedup: rewrite the dirent we already wrote with the
						 * existing inode, drop the freshly allocated child_ino,
						 * and bump i_nlink of the existing inode. */
						cde->d_ino = existing_ino;
						next_ino--; /* rollback the allocation we did above */

						/* Locate existing inode in it_buf and increment nlink */
						uint64_t e_slot = existing_ino - 1;
						uint64_t e_blk_idx = e_slot / inodes_per_block;
						uint64_t e_blk_off = e_slot % inodes_per_block;
						struct beamfs_inode *ei = (struct beamfs_inode *)
						    (it_buf + e_blk_idx * BEAMFS_BLOCK_SIZE
						            + e_blk_off * sizeof(struct beamfs_inode));
						ei->i_nlink++;
						ei->i_crc32 = crc32_inode(ei);
						rs_encode_inode((uint8_t *)ei);

						hardlinks_dedup++;
						continue; /* next entry */
					}
					/* First sighting of this dev/ino: record in htab.
					 * Note: child_ino was allocated for us above. */
					if (htab_n >= MAX_HTAB_ENTRIES) {
						fprintf(stderr, "mkfs.beamfs --from-dir: htab full (max %d unique hardlinked inodes)\n",
							MAX_HTAB_ENTRIES);
						free(htab); free(it_buf);
						return 1;
					}
					htab[htab_n].dev = cst.st_dev;
					htab[htab_n].ino = cst.st_ino;
					htab[htab_n].beamfs_ino = child_ino;
					htab_n++;
				}

				/*
				 * Phase 3d: cascade direct/indirect/dindirect for any
				 * REG of any size (kernel supports up to ~478 GiB per file
				 * via tindirect; we cap on bitmap[0] for Phase 3d:
				 * max ~1908 data blocks total which is ~7.4 MiB user data,
				 * sufficient for any single file in a typical rootfs).
				 */
				uint64_t fsize = (uint64_t)cst.st_size;
				uint64_t per_block = interleave
						   ? BEAMFS_CAPSULE_DATA_BYTES
						   : BEAMFS_DATA_INLINE_BYTES;
				uint64_t nblocks = (fsize + per_block - 1) / per_block;
				if (nblocks == 0) nblocks = 1; /* empty file: 1 data block (might be zero-size; skip alloc?) */
				if (fsize == 0) nblocks = 0; /* truly empty file: no data block */

				/* Phase 4b: cascade capacity now includes dindirect (12 + 512 + 262144 = 262668).
				 * Tindirect (additional 134M blocks) deferred since no rootfs file > 1 GiB. */
				uint64_t cascade_cap_blocks =
				    BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS + BEAMFS_DINDIRECT_PTRS;
				if (nblocks > cascade_cap_blocks) {
					fprintf(stderr, "mkfs.beamfs --from-dir: skipping %s (size %lu bytes => %lu blocks > %lu, Phase 4b cap; tindirect would be needed)\n",
						child_path, (unsigned long)fsize,
						(unsigned long)nblocks,
						(unsigned long)cascade_cap_blocks);
					/* Rollback dirent slot allocated above */
					cur->dirent_slot -= cde->d_rec_len;
					memset(cde, 0, sizeof(struct beamfs_dir_entry));
					next_ino--;
					continue;
				}

				int src_fd = -1;
				if (fsize > 0) {
					src_fd = open(child_path, O_RDONLY);
					if (src_fd < 0) {
						fprintf(stderr, "mkfs.beamfs --from-dir: open %s: %s\n",
							child_path, strerror(errno));
						free(it_buf);
						return 1;
					}
				}

				/* Write inode header fields first; i_direct/i_indirect filled below */
				memset(ci, 0, sizeof(struct beamfs_inode));
				ci->i_mode      = (uint16_t)cst.st_mode;
				ci->i_nlink     = 1;
				ci->i_uid       = (uint32_t)cst.st_uid;
				ci->i_gid       = (uint32_t)cst.st_gid;
				ci->i_size      = fsize;
				{
					const char *sde = getenv("SOURCE_DATE_EPOCH");
					if (sde && *sde) {
						uint64_t ens = (uint64_t)atoll(sde) * 1000000000ULL;
						ci->i_atime = ens; ci->i_mtime = ens; ci->i_ctime = ens;
					} else {
						ci->i_atime = (uint64_t)cst.st_atim.tv_sec * 1000000000ULL + cst.st_atim.tv_nsec;
						ci->i_mtime = (uint64_t)cst.st_mtim.tv_sec * 1000000000ULL + cst.st_mtim.tv_nsec;
						ci->i_ctime = (uint64_t)cst.st_ctim.tv_sec * 1000000000ULL + cst.st_ctim.tv_nsec;
					}
				}

				/* Indirect / dindirect state (Phase 4b).
				 * - indirect_buf  : if nblocks > 12, single block of 512 __le64 ptrs to data blocks
				 * - dind_l1_buf   : if nblocks > 524, single block of 512 __le64 ptrs to L2 blocks
				 * - dind_l2_bufs  : up to 512 L2 blocks, allocated lazily on first L2-slot use.
				 *                   Each L2 block holds 512 __le64 ptrs to data blocks.
				 * - dind_l2_blkno : on-disk block number assigned to each used L2 (parallel array).
				 * - dind_l2_used  : flag per L2 slot, set when first written-to in dind_l2_bufs.
				 * Memory cap: 512 * 4096 = 2 MiB worst case (only used by 1+ GiB files;
				 * a 48 MiB file uses only 1 L2 since (48 MiB / 3824) - 524 ~= 12625, < 512). */
				uint8_t indirect_buf[BEAMFS_BLOCK_SIZE];
				uint64_t indirect_blk = 0;
				int need_indirect = (nblocks > BEAMFS_DIRECT_BLOCKS);

				uint8_t dind_l1_buf[BEAMFS_BLOCK_SIZE];
				uint64_t dind_l1_blk = 0;
				int need_dindirect = (nblocks > (BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS));
				uint8_t (*dind_l2_bufs)[BEAMFS_BLOCK_SIZE] = NULL;
				uint64_t *dind_l2_blkno = NULL;
				uint8_t *dind_l2_used = NULL;
				if (need_dindirect) {
					memset(dind_l1_buf, 0, sizeof(dind_l1_buf));
					dind_l2_bufs = calloc(BEAMFS_INDIRECT_PTRS, BEAMFS_BLOCK_SIZE);
					dind_l2_blkno = calloc(BEAMFS_INDIRECT_PTRS, sizeof(uint64_t));
					dind_l2_used = calloc(BEAMFS_INDIRECT_PTRS, sizeof(uint8_t));
					if (!dind_l2_bufs || !dind_l2_blkno || !dind_l2_used) {
						fprintf(stderr, "mkfs.beamfs --from-dir: OOM dindirect L2 state for %s\n", child_path);
						free(dind_l2_bufs); free(dind_l2_blkno); free(dind_l2_used);
						if (src_fd >= 0) close(src_fd);
						free(it_buf);
						return 1;
					}
				}
				if (need_indirect) {
					memset(indirect_buf, 0, sizeof(indirect_buf));
					indirect_blk = next_blk++;
					if (indirect_blk >= (uint64_t)(bitmap_blocks_count * BEAMFS_BITMAP_BITS_PER_BLOCK)) {
						fprintf(stderr, "mkfs.beamfs --from-dir: indirect_blk %lu beyond Phase 3d bitmap[0]\n",
							(unsigned long)indirect_blk);
						if (src_fd >= 0) close(src_fd);
						free(it_buf);
						return 1;
					}
					/* Mark bitmap */
					{
						uint64_t bit = indirect_blk - data_start_blk;
						uint64_t bmap_idx = bit / BEAMFS_BITMAP_BITS_PER_BLOCK;
						uint64_t bit_in_bmap = bit % BEAMFS_BITMAP_BITS_PER_BLOCK;
						uint64_t sub = bit_in_bmap / (BEAMFS_SUBBLOCK_DATA * 8);
						uint64_t bit_in_sub = bit_in_bmap % (BEAMFS_SUBBLOCK_DATA * 8);
						uint64_t byte_in_sub = bit_in_sub / 8;
						unsigned bit_in_byte = (unsigned)(bit_in_sub % 8);
						uint8_t *bp = bitmap_buf
						              + bmap_idx * BEAMFS_BLOCK_SIZE
						              + sub * BEAMFS_SUBBLOCK_TOTAL
						              + byte_in_sub;
						*bp &= (uint8_t)~(1u << bit_in_byte);
					}
					ci->i_indirect = indirect_blk;
				}

				/* Phase 4b: allocate dindirect L1 block if needed.
				 * L2 blocks allocated lazily per slot inside the data block loop. */
				if (need_dindirect) {
					dind_l1_blk = next_blk++;
					if (dind_l1_blk >= (uint64_t)(bitmap_blocks_count * BEAMFS_BITMAP_BITS_PER_BLOCK)) {
						fprintf(stderr, "mkfs.beamfs --from-dir: dind_l1_blk %lu beyond bitmap range\n",
							(unsigned long)dind_l1_blk);
						free(dind_l2_bufs); free(dind_l2_blkno); free(dind_l2_used);
						if (src_fd >= 0) close(src_fd);
						free(it_buf);
						return 1;
					}
					/* Mark bitmap for L1 */
					{
						uint64_t bit = dind_l1_blk - data_start_blk;
						uint64_t bmap_idx = bit / BEAMFS_BITMAP_BITS_PER_BLOCK;
						uint64_t bit_in_bmap = bit % BEAMFS_BITMAP_BITS_PER_BLOCK;
						uint64_t sub = bit_in_bmap / (BEAMFS_SUBBLOCK_DATA * 8);
						uint64_t bit_in_sub = bit_in_bmap % (BEAMFS_SUBBLOCK_DATA * 8);
						uint64_t byte_in_sub = bit_in_sub / 8;
						unsigned bit_in_byte = (unsigned)(bit_in_sub % 8);
						uint8_t *bp = bitmap_buf
						              + bmap_idx * BEAMFS_BLOCK_SIZE
						              + sub * BEAMFS_SUBBLOCK_TOTAL
						              + byte_in_sub;
						*bp &= (uint8_t)~(1u << bit_in_byte);
					}
					ci->i_dindirect = dind_l1_blk;
				}

				/* Allocate & write data blocks */
				for (uint64_t b = 0; b < nblocks; b++) {
					uint8_t user_data[BEAMFS_DATA_INLINE_BYTES];
					memset(user_data, 0, sizeof(user_data));
					uint64_t cap = interleave
						     ? BEAMFS_CAPSULE_DATA_BYTES
						     : BEAMFS_DATA_INLINE_BYTES;
					ssize_t want = (ssize_t)cap;
					uint64_t remaining = fsize - b * cap;
					if (remaining < (uint64_t)want)
						want = (ssize_t)remaining;
					ssize_t got = read(src_fd, user_data, want);
					if (got != want) {
						fprintf(stderr, "mkfs.beamfs --from-dir: short read %s at block %lu\n",
							child_path, (unsigned long)b);
						close(src_fd);
						free(it_buf);
						return 1;
					}

					uint8_t data_block_buf[BEAMFS_BLOCK_SIZE];
					memset(data_block_buf, 0, sizeof(data_block_buf));
					if (interleave) {
						/*
						 * A capsule holds its payload
						 * contiguously and the encoder
						 * gathers each codeword from
						 * every sixteenth byte.
						 *
						 * Laying it out in sixteen runs
						 * 255 apart and then encoding it
						 * as a capsule wrote a rootfs the
						 * kernel could not read: "ino=1217
						 * subblock=1 uncorrectable" and a
						 * panic at /sbin/init, on a volume
						 * fsck called clean -- because fsck
						 * checks the tree, not the bytes.
						 */
						memcpy(data_block_buf, user_data,
						       BEAMFS_CAPSULE_DATA_BYTES);
						stamp_capsule_csum(data_block_buf,
								   user_data,
								   data_csum,
								   child_ino, b);
						rs_encode_capsule(data_block_buf);
					} else {
						for (int s = 0; s < BEAMFS_DATA_INLINE_SUBBLOCKS; s++) {
							memcpy(data_block_buf + s * BEAMFS_SUBBLOCK_TOTAL,
							       user_data + s * BEAMFS_SUBBLOCK_DATA,
							       BEAMFS_SUBBLOCK_DATA);
						}
						rs_encode_bitmap(data_block_buf);
						stamp_data_csum(data_block_buf, user_data, data_csum,
							child_ino, b);
					}

					uint64_t blk = next_blk++;
					if (blk >= (uint64_t)(bitmap_blocks_count * BEAMFS_BITMAP_BITS_PER_BLOCK)) {
						fprintf(stderr, "mkfs.beamfs --from-dir: data block %lu beyond Phase 3d bitmap[0]\n",
							(unsigned long)blk);
						close(src_fd);
						free(it_buf);
						return 1;
					}
					write_block(fd, blk, data_block_buf);
					/* Mark bitmap */
					{
						uint64_t bit = blk - data_start_blk;
						uint64_t bmap_idx = bit / BEAMFS_BITMAP_BITS_PER_BLOCK;
						uint64_t bit_in_bmap = bit % BEAMFS_BITMAP_BITS_PER_BLOCK;
						uint64_t sub = bit_in_bmap / (BEAMFS_SUBBLOCK_DATA * 8);
						uint64_t bit_in_sub = bit_in_bmap % (BEAMFS_SUBBLOCK_DATA * 8);
						uint64_t byte_in_sub = bit_in_sub / 8;
						unsigned bit_in_byte = (unsigned)(bit_in_sub % 8);
						uint8_t *bp = bitmap_buf
						              + bmap_idx * BEAMFS_BLOCK_SIZE
						              + sub * BEAMFS_SUBBLOCK_TOTAL
						              + byte_in_sub;
						*bp &= (uint8_t)~(1u << bit_in_byte);
					}

					/* Place pointer (Phase 4b cascade):
					 *   b < 12              -> i_direct[b]
					 *   12 <= b < 12+512    -> indirect_buf[b - 12]
					 *   12+512 <= b         -> dind_l2_bufs[(b - 524) / 512][((b - 524) % 512)]
					 *                          with dind_l2_blkno[(b - 524)/512] populated lazily
					 *                          + dind_l1_buf[l1_slot] = dind_l2_blkno[l1_slot]
					 */
					if (b < BEAMFS_DIRECT_BLOCKS) {
						ci->i_direct[b] = blk;
					} else if (b < BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS) {
						uint64_t idx = b - BEAMFS_DIRECT_BLOCKS;
						((uint64_t *)indirect_buf)[idx] = blk;
					} else {
						uint64_t idx_dind = b - BEAMFS_DIRECT_BLOCKS - BEAMFS_INDIRECT_PTRS;
						uint64_t l1_slot = idx_dind / BEAMFS_INDIRECT_PTRS;
						uint64_t l2_slot = idx_dind % BEAMFS_INDIRECT_PTRS;
						/* Lazy-allocate L2 block on first use of this l1_slot */
						if (!dind_l2_used[l1_slot]) {
							uint64_t l2_blk = next_blk++;
							if (l2_blk >= (uint64_t)(bitmap_blocks_count * BEAMFS_BITMAP_BITS_PER_BLOCK)) {
								fprintf(stderr, "mkfs.beamfs --from-dir: dindirect L2 block %lu beyond bitmap range\n",
									(unsigned long)l2_blk);
								free(dind_l2_bufs); free(dind_l2_blkno); free(dind_l2_used);
								if (src_fd >= 0) close(src_fd);
								free(it_buf);
								return 1;
							}
							dind_l2_blkno[l1_slot] = l2_blk;
							dind_l2_used[l1_slot] = 1;
							/* Mark bitmap for this L2 block */
							{
								uint64_t bit = l2_blk - data_start_blk;
								uint64_t bmap_idx = bit / BEAMFS_BITMAP_BITS_PER_BLOCK;
								uint64_t bit_in_bmap = bit % BEAMFS_BITMAP_BITS_PER_BLOCK;
								uint64_t sub = bit_in_bmap / (BEAMFS_SUBBLOCK_DATA * 8);
								uint64_t bit_in_sub = bit_in_bmap % (BEAMFS_SUBBLOCK_DATA * 8);
								uint64_t byte_in_sub = bit_in_sub / 8;
								unsigned bit_in_byte = (unsigned)(bit_in_sub % 8);
								uint8_t *bp = bitmap_buf
								              + bmap_idx * BEAMFS_BLOCK_SIZE
								              + sub * BEAMFS_SUBBLOCK_TOTAL
								              + byte_in_sub;
								*bp &= (uint8_t)~(1u << bit_in_byte);
							}
							/* Record in L1 buffer */
							((uint64_t *)dind_l1_buf)[l1_slot] = l2_blk;
						}
						/* Store data block addr in the L2 buffer */
						((uint64_t *)dind_l2_bufs[l1_slot])[l2_slot] = blk;
					}
				}
				if (src_fd >= 0) close(src_fd);

				/* Write indirect block if used */
				if (need_indirect) {
					write_block(fd, indirect_blk, indirect_buf);
					write_ind_parity(fd, indirect_blk, indirect_buf,
							 data_start_blk, ind_parity_blk,
							 (uint32_t)ind_parity_count, ind_parity_mode);
				}

				/* Phase 4b: write dindirect L2 blocks used + L1 block */
				if (need_dindirect) {
					for (uint64_t l1 = 0; l1 < BEAMFS_INDIRECT_PTRS; l1++) {
						if (dind_l2_used[l1]) {
							write_block(fd, dind_l2_blkno[l1], dind_l2_bufs[l1]);
							write_ind_parity(fd, dind_l2_blkno[l1], dind_l2_bufs[l1],
									 data_start_blk, ind_parity_blk,
									 (uint32_t)ind_parity_count, ind_parity_mode);
						}
					}
					write_block(fd, dind_l1_blk, dind_l1_buf);
					write_ind_parity(fd, dind_l1_blk, dind_l1_buf,
							 data_start_blk, ind_parity_blk,
							 (uint32_t)ind_parity_count, ind_parity_mode);
					free(dind_l2_bufs);
					free(dind_l2_blkno);
					free(dind_l2_used);
				}

				/* Finalize inode CRC + RS */
				ci->i_crc32 = crc32_inode(ci);
				rs_encode_inode((uint8_t *)ci);

				files_written++;

			} else if (S_ISDIR(cst.st_mode)) {
				cde->d_file_type = 4; /* DT_DIR */

				if (sp >= MAX_DFS_DEPTH) {
					fprintf(stderr, "mkfs.beamfs --from-dir: max DFS depth %d exceeded at %s\n",
						MAX_DFS_DEPTH, child_path);
					free(it_buf);
					return 1;
				}

				/* Allocate dirblock for this dir */
				uint64_t child_dir_blk = next_blk++;
				if (child_dir_blk >= (uint64_t)(bitmap_blocks_count * BEAMFS_BITMAP_BITS_PER_BLOCK)) {
					fprintf(stderr, "mkfs.beamfs --from-dir: dirblock %lu beyond Phase 3c bitmap[0] range\n",
						(unsigned long)child_dir_blk);
					free(it_buf);
					return 1;
				}
				/* Mark bit */
				{
					uint64_t bit = child_dir_blk - data_start_blk;
					uint64_t bmap_idx = bit / BEAMFS_BITMAP_BITS_PER_BLOCK;
					uint64_t bit_in_bmap = bit % BEAMFS_BITMAP_BITS_PER_BLOCK;
					uint64_t sub = bit_in_bmap / (BEAMFS_SUBBLOCK_DATA * 8);
					uint64_t bit_in_sub = bit_in_bmap % (BEAMFS_SUBBLOCK_DATA * 8);
					uint64_t byte_in_sub = bit_in_sub / 8;
					unsigned bit_in_byte = (unsigned)(bit_in_sub % 8);
					uint8_t *bp = bitmap_buf
					              + bmap_idx * BEAMFS_BLOCK_SIZE
					              + sub * BEAMFS_SUBBLOCK_TOTAL
					              + byte_in_sub;
					*bp &= (uint8_t)~(1u << bit_in_byte);
				}

				/* Write inode (will be patched at finalization for nlink+size) */
				ci->i_mode      = (uint16_t)cst.st_mode;
				ci->i_nlink     = 2; /* . + parent's reference; subdirs add via finalize */
				ci->i_uid       = (uint32_t)cst.st_uid;
				ci->i_gid       = (uint32_t)cst.st_gid;
				ci->i_size      = interleave
						? BEAMFS_CAPSULE_DATA_BYTES
						: BEAMFS_DATA_INLINE_BYTES;
				{
					const char *sde = getenv("SOURCE_DATE_EPOCH");
					if (sde && *sde) {
						uint64_t ens = (uint64_t)atoll(sde) * 1000000000ULL;
						ci->i_atime = ens; ci->i_mtime = ens; ci->i_ctime = ens;
					} else {
						ci->i_atime = (uint64_t)cst.st_atim.tv_sec * 1000000000ULL + cst.st_atim.tv_nsec;
						ci->i_mtime = (uint64_t)cst.st_mtim.tv_sec * 1000000000ULL + cst.st_mtim.tv_nsec;
						ci->i_ctime = (uint64_t)cst.st_ctim.tv_sec * 1000000000ULL + cst.st_ctim.tv_nsec;
					}
				}
				ci->i_flags     = 0;
				ci->i_direct[0] = child_dir_blk;

				/* Push child frame (Phase 4c multi-block dirs) */
				stack[sp].path = strdup(child_path);
				stack[sp].dir_ino = child_ino;
				stack[sp].parent_ino = cur->dir_ino;
				stack[sp].n_dir_blocks = 1;
				stack[sp].dir_blk_nums = calloc(BEAMFS_DIRECT_BLOCKS + BEAMFS_INDIRECT_PTRS, sizeof(uint64_t));
				stack[sp].dir_blocks_buf = calloc(1, BEAMFS_BLOCK_SIZE);
				stack[sp].dir_indirect_blk = 0;
				stack[sp].dir_indirect_buf = NULL;
				if (!stack[sp].dir_blk_nums || !stack[sp].dir_blocks_buf) {
					fprintf(stderr, "mkfs.beamfs --from-dir: OOM child frame\n");
					free(it_buf);
					return 1;
				}
				stack[sp].dir_blk_nums[0] = child_dir_blk;

				/* Seed first dirblock with . and .. */
				struct beamfs_dir_entry *de_dot = (struct beamfs_dir_entry *)stack[sp].dir_blocks_buf;
				de_dot->d_ino       = child_ino;
				de_dot->d_name_len  = 1;
				de_dot->d_file_type = 4; /* DT_DIR */
				de_dot->d_name[0]   = '.';
				de_dot->d_rec_len   = BEAMFS_DIRENT_LEN(1);

				struct beamfs_dir_entry *de_dotdot = (struct beamfs_dir_entry *)
				    (stack[sp].dir_blocks_buf + BEAMFS_DIRENT_LEN(1));
				de_dotdot->d_ino       = cur->dir_ino;
				de_dotdot->d_name_len  = 2;
				de_dotdot->d_file_type = 4;
				de_dotdot->d_name[0]   = '.';
				de_dotdot->d_name[1]   = '.';
				de_dotdot->d_rec_len   = BEAMFS_DIRENT_LEN(2);

				stack[sp].dirent_slot = BEAMFS_DIRENT_LEN(1) +
						        BEAMFS_DIRENT_LEN(2);
				stack[sp].subdir_count = 0;
				stack[sp].is_root = 0;
				stack[sp].names = NULL;
				stack[sp].n_names = 0;
				stack[sp].name_idx = 0;
				sp++;

				/* Note: inode crc32/rs are computed at FINALIZE time when
				 * we know nlink. */
				dirs_written++;

			} else if (S_ISLNK(cst.st_mode)) {
				cde->d_file_type = 10; /* DT_LNK */

				/*
				 * Phase 3e: fast symlink only (kernel namei.c::beamfs_symlink
				 * rejects >= 96 bytes with ENAMETOOLONG). Target stored
				 * inline in i_direct[0..11] (96 bytes), zero-padded.
				 * i_size = strlen(target).
				 * i_mode forced to S_IFLNK | 0777 (Linux convention).
				 */
				char tgt[4096];
				ssize_t tlen = readlink(child_path, tgt, sizeof(tgt) - 1);
				if (tlen < 0) {
					fprintf(stderr, "mkfs.beamfs --from-dir: readlink %s: %s\n",
						child_path, strerror(errno));
					free(it_buf);
					return 1;
				}
				tgt[tlen] = '\0';

				if ((size_t)tlen >= sizeof(ci->i_direct)) {
					fprintf(stderr, "mkfs.beamfs --from-dir: skipping %s (symlink target %zu bytes >= 96; kernel rejects with ENAMETOOLONG)\n",
						child_path, (size_t)tlen);
					/* Rollback dirent + ino */
					cur->dirent_slot -= cde->d_rec_len;
					memset(cde, 0, sizeof(struct beamfs_dir_entry));
					next_ino--;
					continue;
				}

				memset(ci, 0, sizeof(struct beamfs_inode));
				ci->i_mode      = (uint16_t)(S_IFLNK | 0777);
				ci->i_nlink     = 1;
				ci->i_uid       = (uint32_t)cst.st_uid;
				ci->i_gid       = (uint32_t)cst.st_gid;
				ci->i_size      = (uint64_t)tlen;
				{
					const char *sde = getenv("SOURCE_DATE_EPOCH");
					if (sde && *sde) {
						uint64_t ens = (uint64_t)atoll(sde) * 1000000000ULL;
						ci->i_atime = ens; ci->i_mtime = ens; ci->i_ctime = ens;
					} else {
						ci->i_atime = (uint64_t)cst.st_atim.tv_sec * 1000000000ULL + cst.st_atim.tv_nsec;
						ci->i_mtime = (uint64_t)cst.st_mtim.tv_sec * 1000000000ULL + cst.st_mtim.tv_nsec;
						ci->i_ctime = (uint64_t)cst.st_ctim.tv_sec * 1000000000ULL + cst.st_ctim.tv_nsec;
					}
				}
				ci->i_flags = 0;
				/* Inline target into i_direct[] reused as 96-byte char buffer */
				memcpy((char *)ci->i_direct, tgt, tlen);
				ci->i_crc32 = crc32_inode(ci);
				rs_encode_inode((uint8_t *)ci);

				files_written++; /* count LNK as a file for stats simplicity */
			}
		}

		/* Phase 4a: write back all K bitmap blocks. rs_encode_bitmap mutates
		 * the buffer in place (16 parity bytes per subblock written), so we
		 * encode then write each block once. */
		for (uint64_t k = 0; k < bitmap_blocks_count; k++) {
			uint8_t *bp_k = bitmap_buf + k * BEAMFS_BLOCK_SIZE;
			rs_encode_bitmap(bp_k);
			write_block(fd, bitmap_blk + k, bp_k);
		}

		/* Patch root inode: it lives in it_buf block 0 slot 0 (already
		 * a copy of inode_block, but in Phase 3c the root may have new
		 * subdirs => bump nlink). The root frame was popped at end of DFS;
		 * its subdir_count is captured in root_subdir_count via the
		 * propagation logic. We retrieve it from the original stack[0]
		 * but it's already freed; instead we re-derive from counters.
		 *
		 * Simpler: when the root frame finalizes (in the loop above), it
		 * patches its own inode in it_buf. The loop's "if non-root, propagate
		 * up" doesn't bump root, but the root finalize code writes
		 * nlink = 2 + cur->subdir_count itself. So root nlink is already
		 * correct in it_buf. */
		(void)root_subdir_count; /* unused (kept for clarity) */

		/* Write all inode table blocks */
		for (uint64_t b = 0; b < inode_table_len; b++) {
			write_block(fd, inode_table_blk + b,
				it_buf + b * BEAMFS_BLOCK_SIZE);
		}
		free(it_buf);

		/* Update sb counters: files + dirs consumed (next_ino - 1 - existing) */
		uint64_t used_ino = next_ino - (canary_present ? 3 : 2);
		uint64_t used_blk = next_blk - data_start_blk;
		sb.s_free_inodes -= used_ino;
		sb.s_free_blocks -= used_blk;

		printf("  --from-dir:  %lu file(s), %lu dir(s), %lu hardlink dedup from %s (inodes used=%lu, blocks used=%lu)\n",
		       (unsigned long)files_written, (unsigned long)dirs_written,
		       (unsigned long)hardlinks_dedup,
		       from_dir, (unsigned long)used_ino, (unsigned long)used_blk);
		free(htab);
	}

	/* Phase 4a: if --from-dir was NOT used, the canary mark in bitmap_buf
	 * is still in memory only. Flush all K bitmap blocks to disk now. */
	if (!from_dir) {
		for (uint64_t k = 0; k < bitmap_blocks_count; k++) {
			uint8_t *bp_k = bitmap_buf + k * BEAMFS_BLOCK_SIZE;
			rs_encode_bitmap(bp_k);
			write_block(fd, bitmap_blk + k, bp_k);
		}
	}


	/*
	 * Stage 3 item 2: encode RS parity over the CRC32-covered region
	 * before computing the CRC. The parity sits in s_pad which is
	 * outside the CRC32 coverage; the order RS-first vs CRC-last
	 * does not affect the s_crc32 value, but is the convention
	 * mirrored on the kernel side (beamfs_dirty_super in commit C).
	 */
	rs_encode_super(&sb);
	sb.s_crc32          = crc32_sb(&sb);

	write_block(fd, 0, &sb);
	free(bitmap_buf);
	close(fd);

	printf("mkfs.beamfs: formatted %s\n", argv[optind]);
	printf("  format:  v%u%s%s%s (scheme=%u %s, feat_incompat=0x%016llx%s%s%s, journal entry %zu bytes)\n",
	       (unsigned)sb.s_version,
	       profile_name ? " [" : "",
	       profile_name ? profile_name : "",
	       profile_name ? "]" : "",
	       (unsigned)sb.s_data_protection_scheme,
	       scheme_name,
	       (unsigned long long)sb.s_feat_incompat,
	       (sb.s_feat_incompat & MKFS_FEATURE_INCOMPAT_PER_INODE_RS) ? " [" : "",
	       (sb.s_feat_incompat & MKFS_FEATURE_INCOMPAT_PER_INODE_RS) ? "per_inode_rs" : "",
	       (sb.s_feat_incompat & MKFS_FEATURE_INCOMPAT_PER_INODE_RS) ? "]" : "",
	       sizeof(struct beamfs_rs_event));
	printf("  blocks:  %lu (free: %lu)\n",
	       (unsigned long)total_blocks,
	       (unsigned long)sb.s_free_blocks);
	printf("  inodes:  %lu (free: %lu)\n",
	       (unsigned long)total_inodes,
	       (unsigned long)sb.s_free_inodes);
	printf("  inode table: block %lu\n", (unsigned long)inode_table_blk);
	printf("  bitmap:      block %lu .. %lu (%lu blocks, RS FEC protected)\n",
	      (unsigned long)bitmap_blk,
	      (unsigned long)(bitmap_blk + bitmap_blocks_count - 1),
	      (unsigned long)bitmap_blocks_count);
	printf("  data start:  block %lu\n", (unsigned long)data_start_blk);
	if (canary_present) {
		printf("  canary:      block %lu inode 2 (on-disk fixture, no directory entry, format-v4.md sec 11)\n",
		       (unsigned long)canary_blk);
	}
	return 0;
}
