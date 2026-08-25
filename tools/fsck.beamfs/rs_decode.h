/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fsck.beamfs - Reed-Solomon decoder API (userland)
 *
 * Shortened RS(255,239) decoder for the beamfs on-disk subblock
 * layout: data segment (variable length 172/211/239) + 16 bytes
 * parity. Codec parameters match the kernel:
 *   init_rs(symsize=8, gfpoly=0x187, fcr=0, prim=1, nroots=16)
 *
 * Sub-phase 2 (sub-2.partial-2): API + test vectors skeleton.
 * The decoder body is a userland port of lib/reed_solomon/{reed_solomon,
 * decode_rs}.c (kernel.org, GPL-2.0); landing in a follow-up commit.
 * Until then, rs_decode_subblock() returns RS_NOT_IMPLEMENTED.
 *
 * Author: roastercode - Aurelien DESBRIERES <aurelien@hackers.camp>
 */

#ifndef FSCK_BEAMFS_RS_DECODE_H
#define FSCK_BEAMFS_RS_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Return codes for rs_decode_subblock */
#define RS_OK              0    /* data was already a codeword */
#define RS_CORRECTED       1    /* >0 in real impl: returns N corrected symbols */
#define RS_UNCORRECTABLE  -1    /* errors beyond RS correction radius */
#define RS_NOT_IMPLEMENTED -2   /* skeleton placeholder (sub-2.partial-2) */

/* RS codec parameters (kernel beamfs match). */
#define RS_SYMSIZE  8
#define RS_GFPOLY   0x187
#define RS_FCR      0
#define RS_PRIM     1
#define RS_NROOTS   16

/* Maximum supported data segment length (RS(255,239) -> 239 max) */
#define RS_MAX_DATA_LEN  239

/* Opaque codec handle. Allocated by rs_init(), freed by rs_free(). */
struct rs_codec;

/*
 * rs_init -- allocate and initialise the RS codec.
 * Returns NULL on allocation failure or invalid params.
 * Lifetime: passed to all rs_decode_subblock() calls, freed via rs_free().
 */
struct rs_codec *rs_init(void);

/*
 * rs_free -- release the codec.
 */
void rs_free(struct rs_codec *rs);

/*
 * rs_decode_subblock -- in-place decode of one shortened subblock.
 *
 * @rs:        codec handle from rs_init()
 * @data:      data segment, len bytes, modified in place if corrections apply
 * @len:       data length (172 for inode, 211 for SB sub-block, 239 for bitmap)
 * @parity:    16 bytes of RS parity (separate buffer)
 * @positions: optional output array sized RS_NROOTS / 2 bytes minimum;
 *             on success returns the data positions corrected (NULL = ignore)
 *
 * Return:  >= 0 number of symbols corrected (RS_OK = 0 means clean codeword)
 *          RS_UNCORRECTABLE on errors beyond correction radius
 *          RS_NOT_IMPLEMENTED in sub-2.partial-2 skeleton
 */
int rs_decode_subblock(struct rs_codec *rs,
		       uint8_t *data, size_t len,
		       uint8_t *parity,
		       int *positions);

/*
 * rs_encode_subblock -- compute RS(255,239) parity for one shortened
 * subblock, matching the kernel's beamfs_rs_encode() and mkfs.beamfs's
 * encode_rs_userspace() byte-for-byte (same codec parameters, same
 * generator-polynomial LFSR encoding).
 *
 * @rs:      codec handle from rs_init()
 * @data:    data segment, len bytes, read-only
 * @len:     data length (172 for inode, 211 for SB sub-block, 239 for
 *           bitmap/data block)
 * @parity:  output, 16 bytes
 *
 * Needed wherever a pass modifies bytes inside an RS-covered region and
 * must re-encode before writing back (pass 5's journal repair modifies
 * s_rs_journal[], which lives inside the superblock's RS-covered
 * region B).
 */
void rs_encode_subblock(struct rs_codec *rs,
			const uint8_t *data, size_t len,
			uint8_t *parity);

#endif /* FSCK_BEAMFS_RS_DECODE_H */
