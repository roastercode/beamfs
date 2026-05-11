/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fsck.beamfs - CRC32 helpers (userland)
 *
 * CRC-32/ISO-HDLC (standard, same as ext4/btrfs and the kernel
 * beamfs module via crc32_le with init 0xFFFFFFFF + final XOR).
 * Byte-exact match with mkfs.beamfs and the in-kernel
 * beamfs_crc32() so that on-disk CRCs validate uniformly.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#ifndef FSCK_BEAMFS_CRC32_H
#define FSCK_BEAMFS_CRC32_H

#include <stddef.h>
#include <stdint.h>

struct beamfs_super_block;

/*
 * crc32_internal -- chainable CRC32, returns raw internal state
 * (no final XOR). Use 0xFFFFFFFF as seed for the first block,
 * carry the return value for subsequent chained regions.
 */
uint32_t crc32_internal(uint32_t seed, const void *buf, size_t len);

/*
 * crc32 -- one-shot CRC-32/ISO-HDLC over a single buffer.
 * Equivalent to crc32_internal(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF.
 */
uint32_t crc32(const void *buf, size_t len);

/*
 * crc32_sb -- CRC32 over the meaningful regions of the
 * superblock, excluding s_crc32 itself and s_pad.
 *
 * Coverage (computed at compile time from struct layout):
 *   region A: [0, offsetof(s_crc32))         -- magic, counters,
 *                                               version, flags
 *   region B: [offsetof(s_uuid), offsetof(s_pad)) -- uuid, label,
 *                                               RS journal,
 *                                               bitmap_blk, features,
 *                                               protection scheme
 * Total coverage: BEAMFS_SB_RS_COVERAGE_BYTES (2709 bytes).
 *
 * Must match the kernel beamfs_crc32_sb() byte-for-byte.
 */
uint32_t crc32_sb(const struct beamfs_super_block *sb);

#endif /* FSCK_BEAMFS_CRC32_H */
