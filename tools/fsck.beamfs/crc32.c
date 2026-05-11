// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsck.beamfs - CRC32 helpers (userland implementation)
 *
 * Table-based CRC-32/ISO-HDLC, polynomial 0xEDB88320 (reflected),
 * init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Identical to ext4/btrfs
 * and the kernel crc32_le used by beamfs_crc32().
 *
 * Source: copy of mkfs.beamfs.c CRC32 helpers, kept in lockstep.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#include <stddef.h>
#include <stdint.h>

#include "crc32.h"
#include "sb_layout.h"

static uint32_t crc32_table[256];
static int      crc32_init_done;

static void crc32_init(void)
{
	uint32_t poly = 0xEDB88320;
	int i, j;

	for (i = 0; i < 256; i++) {
		uint32_t c = i;

		for (j = 0; j < 8; j++)
			c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
		crc32_table[i] = c;
	}
	crc32_init_done = 1;
}

uint32_t crc32_internal(uint32_t seed, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint32_t       c = seed;

	if (!crc32_init_done)
		crc32_init();
	while (len--)
		c = crc32_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return c;
}

uint32_t crc32(const void *buf, size_t len)
{
	return crc32_internal(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

uint32_t crc32_sb(const struct beamfs_super_block *sb)
{
	const uint8_t *base       = (const uint8_t *)sb;
	const size_t   off_crc32  = offsetof(struct beamfs_super_block, s_crc32);
	const size_t   off_uuid   = offsetof(struct beamfs_super_block, s_uuid);
	const size_t   off_pad    = offsetof(struct beamfs_super_block, s_pad);
	uint32_t       c;

	static_assert(sizeof(((struct beamfs_super_block *)0)->s_crc32) == 4,
		      "s_crc32 must be 4 bytes");

	c = crc32_internal(0xFFFFFFFF, base, off_crc32);
	c = crc32_internal(c, base + off_uuid, off_pad - off_uuid);
	return c ^ 0xFFFFFFFF;
}
