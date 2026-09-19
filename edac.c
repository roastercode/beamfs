// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - EDAC layer: CRC32 + Reed-Solomon FEC
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Reed-Solomon encoding/decoding uses the kernel's lib/reed_solomon
 * library (RS(255,239) over GF(2^8), primitive polynomial 0x187).
 * This avoids duplicating well-tested RS code already present in the
 * kernel (used by NAND MTD, DVB, etc.) and addresses the concern raised
 * during review (Eric Biggers, linux-fsdevel, April 2026).
 *
 * RS parameters:
 *   BEAMFS_RS_SYMSIZE = 8         (GF(2^8))
 *   BEAMFS_RS_FCR     = 0         (first consecutive root)
 *   BEAMFS_RS_PRIM    = 1         (primitive element)
 *   BEAMFS_RS_NROOTS  = BEAMFS_RS_PARITY = 16 (parity symbols)
 *   data per subblock: BEAMFS_SUBBLOCK_DATA = 239 bytes
 *   codeword length:   255 bytes (BEAMFS_SUBBLOCK_TOTAL)
 */

#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/rslib.h>
#include "beamfs.h"

/*
 * RS codec handles -- ONE PER POSSIBLE CPU.
 *
 * lib/reed_solomon/decode_rs.c uses scratch buffers (lambda, syn, b,
 * t, omega, root, reg, loc) stored inside rs_control->buffers[]. A
 * shared rs_control across concurrent callers races on those buffers,
 * producing spurious 'uncorrectable' results under parallel decode
 * load (reproduced May 2026: 8 parallel sha256sum on a beamfs RO
 * mount -> 160+ uncorrectable per batch).
 *
 * Upstream rslib.h documents rs_control as 'per instance' but does
 * NOT state non-thread-safety. Bug latent in mainline lib since 2002.
 *
 * Per-CPU allocation gives each CPU its own rs_control + buffers[].
 * Combined with preempt-disable via get_cpu_ptr/put_cpu_ptr, this
 * eliminates the race without locks.
 */
static struct rs_control * __percpu *beamfs_rs_ctrl_pcpu;

/*
 * A codeword's worth of scratch, per cpu.
 *
 * Interleaving means a codeword's symbols are not contiguous, and
 * encode_rs8 takes a contiguous buffer: lib/reed_solomon has no notion
 * of a stride and is not ours to change. So the symbols are gathered
 * into this and handed over.
 *
 * Per cpu rather than on the stack: 239 bytes is a quarter of a
 * kernel frame, and this sits under iomap and writeback, where the
 * frame is already deep. Per cpu rather than from the scratch pool:
 * this file has no super_block and should not need one -- it is the
 * codec, not the filesystem.
 *
 * Taken with get_cpu_ptr, which disables preemption for the length of
 * one codeword: 239 bytes of Galois arithmetic, microseconds.
 */
static u8 * __percpu *beamfs_rs_scratch_pcpu;

/*
 * beamfs_rs_init_tables - initialize the RS codec
 * Called once from beamfs_init() before any mount.
 */
void beamfs_rs_init_tables(void)
{
	unsigned int cpu;
	struct rs_control *ctrl;

	/*
	 * init_rs(symsize=8, gfpoly=0x187, fcr=0, prim=1, nroots=16)
	 *   -> GF(2^8), primitive poly x^8+x^7+x^2+x+1, 16 parity symbols
	 *      (corrects up to 8 errors per shortened RS(255,239) subblock).
	 *
	 * Per-CPU: alloc the per-CPU pointer slot, then init_rs once per
	 * possible CPU. Each rs_control carries its own scratch buffers[].
	 */
	beamfs_rs_scratch_pcpu = alloc_percpu(u8 *);
	if (!beamfs_rs_scratch_pcpu) {
		/*
		 * Not fatal: the contiguous paths never touch it and the
		 * interleaved ones check before use, so a volume that
		 * needs interleaving refuses rather than the module.
		 */
		pr_err("beamfs: no per-CPU RS scratch; interleaved blocks will be refused\n");
	}

	beamfs_rs_ctrl_pcpu = alloc_percpu(struct rs_control *);
	if (!beamfs_rs_ctrl_pcpu) {
		free_percpu(beamfs_rs_scratch_pcpu);
		beamfs_rs_scratch_pcpu = NULL;
		pr_err("beamfs: failed to alloc per-CPU RS ctrl array\n");
		return;
	}

	for_each_possible_cpu(cpu) {
		ctrl = init_rs(8, 0x187, 0, 1, BEAMFS_RS_PARITY);
		if (!ctrl) {
			pr_err("beamfs: failed to init RS codec for CPU %u\n",
			       cpu);
			beamfs_rs_exit_tables();
			return;
		}
		*per_cpu_ptr(beamfs_rs_ctrl_pcpu, cpu) = ctrl;
		/*
		 * A failure here is not fatal: the contiguous paths do
		 * not use it, and the interleaved ones check.
		 */
		*per_cpu_ptr(beamfs_rs_scratch_pcpu, cpu) =
			kmalloc(BEAMFS_SUBBLOCK_DATA, GFP_KERNEL);
	}

	pr_debug("beamfs: RS codec initialized per-CPU (RS(%d,%d), %u CPUs)\n",
		 BEAMFS_SUBBLOCK_TOTAL, BEAMFS_SUBBLOCK_DATA,
		 num_possible_cpus());
}

/*
 * beamfs_rs_exit - release the RS codec at module exit
 */
void beamfs_rs_exit_tables(void)
{
	unsigned int cpu;
	struct rs_control *ctrl;

	if (!beamfs_rs_ctrl_pcpu)
		return;

	for_each_possible_cpu(cpu) {
		ctrl = *per_cpu_ptr(beamfs_rs_ctrl_pcpu, cpu);
		if (ctrl) {
			free_rs(ctrl);
			*per_cpu_ptr(beamfs_rs_ctrl_pcpu, cpu) = NULL;
		}
	}

	if (beamfs_rs_scratch_pcpu) {
		unsigned int cpu2;

		for_each_possible_cpu(cpu2)
			kfree(*per_cpu_ptr(beamfs_rs_scratch_pcpu, cpu2));
		free_percpu(beamfs_rs_scratch_pcpu);
		beamfs_rs_scratch_pcpu = NULL;
	}

	free_percpu(beamfs_rs_ctrl_pcpu);
	beamfs_rs_ctrl_pcpu = NULL;
}

/*
 * Shannon entropy term LUT in Q16.16 fixed point.
 *
 * Auto-generated by tools/gen_entropy_lut.py -- DO NOT EDIT.
 *
 * LUT[N][k] = round(-(k/N) * log2(k/N) * 2^16) for N in [1,8],
 *           = 0 for k == 0 (or N == 0, unused).
 *
 * To regenerate after a parameter change:
 *   ./tools/gen_entropy_lut.py
 * The output between the SENTINEL markers below should match
 * byte-for-byte.
 */
/* SENTINEL_LUT_BEGIN -- generator hash bf11b7d225cb6f13 */
static const __u32 beamfs_rs_entropy_term_q16_16[9][9] = {
	[0] = { 0, },	/* [0] unused */
	[1] = {     0,     0,     0,     0,     0,     0,     0,     0,     0 },
	[2] = {     0, 32768,     0,     0,     0,     0,     0,     0,     0 },
	[3] = {     0, 34624, 25557,     0,     0,     0,     0,     0,     0 },
	[4] = {     0, 32768, 32768, 20400,     0,     0,     0,     0,     0 },
	[5] = {     0, 30434, 34654, 28979, 16878,     0,     0,     0,     0 },
	[6] = {     0, 28235, 34624, 32768, 25557, 14365,     0,     0,     0 },
	[7] = {     0, 26283, 33842, 34333, 30235, 22724, 12493,     0,     0 },
	[8] = {     0, 24576, 32768, 34776, 32768, 27774, 20400, 11047,     0 },
};
/* SENTINEL_LUT_END */

/*
 * beamfs_rs_compute_entropy_q16_16 -- Shannon entropy estimator over the
 * spatial distribution of corrected byte positions in an RS codeword.
 *
 * @positions:      byte positions corrected (output of beamfs_rs_decode)
 * @n_positions:    number of valid entries; MUST be in [1, BEAMFS_RS_PARITY/2]
 * @code_len_bytes: data length of the codeword, in [n_positions, 239]
 *
 * Returns Shannon H = sum_{bin} LUT[N][k_bin] in Q16.16 fixed point,
 * range [0, 3*65536). Deterministic, no FPU; one u32 division per
 * position for bin index computation.
 *
 * Pre: positions != NULL, 1 <= n_positions <= BEAMFS_RS_PARITY/2,
 *      code_len_bytes >= n_positions
 *
 * Caller decides whether to set BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID:
 * a single sample (n_positions == 1) yields H = 0 mathematically
 * but is forensically non-significant; the caller (beamfs_log_rs_event)
 * clears the flag in that case.
 */
__u32 beamfs_rs_compute_entropy_q16_16(const int *positions,
				      unsigned int n_positions,
				      size_t code_len_bytes)
{
	unsigned int bin_count[BEAMFS_RS_ENTROPY_BINS];
	__u32 h = 0;
	unsigned int i, b;

	if (WARN_ON_ONCE(!positions))
		return 0;
	if (WARN_ON_ONCE(n_positions == 0 ||
			 n_positions > BEAMFS_RS_PARITY / 2))
		return 0;
	if (WARN_ON_ONCE(code_len_bytes < n_positions ||
			 code_len_bytes > BEAMFS_SUBBLOCK_DATA))
		return 0;

	memset(bin_count, 0, sizeof(bin_count));

	for (i = 0; i < n_positions; i++) {
		/* bin = pos * BINS / code_len. u32 product fits since
		 * pos < 239 and BINS = 8, max product = 1912.
		 */
		unsigned int idx = (unsigned int)positions[i];
		unsigned int bin;

		if (WARN_ON_ONCE(idx >= code_len_bytes))
			return 0;
		bin = (idx * BEAMFS_RS_ENTROPY_BINS) /
		      (unsigned int)code_len_bytes;
		if (WARN_ON_ONCE(bin >= BEAMFS_RS_ENTROPY_BINS))
			return 0;
		bin_count[bin]++;
	}

	/* H = sum over bins of LUT[N][k_bin]. Bins with k=0 contribute 0
	 * by LUT convention. Total guaranteed < 3 * 65536.
	 */
	for (b = 0; b < BEAMFS_RS_ENTROPY_BINS; b++)
		h += beamfs_rs_entropy_term_q16_16[n_positions][bin_count[b]];

	return h;
}

/*
 * beamfs_rs_encode - encode @len data bytes, produce BEAMFS_RS_PARITY parity
 * @data:   input data (@len bytes, must be <= BEAMFS_SUBBLOCK_DATA)
 * @len:    number of data bytes (BEAMFS_SUBBLOCK_DATA for the bitmap path,
 *          BEAMFS_INODE_RS_DATA for inodes, etc.)
 * @parity: output parity (BEAMFS_RS_PARITY bytes)
 *
 * lib/reed_solomon supports shortened RS codes natively: passing a
 * length less than BEAMFS_SUBBLOCK_DATA produces a valid codeword,
 * mathematically equivalent to padding the data with zeros up to the
 * full subblock length. The same length must be passed to the matching
 * beamfs_rs_decode call.
 *
 * The kernel encode_rs8 API takes uint8_t *data directly; parity is
 * returned via a uint16_t *par buffer (low byte holds the parity symbol).
 */
int beamfs_rs_encode(uint8_t *data, size_t len, uint8_t *parity)
{
	uint16_t par[BEAMFS_RS_PARITY];
	struct rs_control **ctrl_p;
	struct rs_control *ctrl;
	int i;

	if (!beamfs_rs_ctrl_pcpu)
		return -EINVAL;
	if (len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;

	memset(par, 0, sizeof(par));
	ctrl_p = get_cpu_ptr(beamfs_rs_ctrl_pcpu);
	ctrl = *ctrl_p;
	if (!ctrl) {
		put_cpu_ptr(beamfs_rs_ctrl_pcpu);
		return -EINVAL;
	}
	encode_rs8(ctrl, data, len, par, 0);
	put_cpu_ptr(beamfs_rs_ctrl_pcpu);

	for (i = 0; i < BEAMFS_RS_PARITY; i++)
		parity[i] = (uint8_t)par[i];

	return 0;
}

/*
 * beamfs_rs_decode -- decode and correct a shortened RS codeword in place,
 *                    optionally exposing the list of corrected byte
 *                    positions for forensic entropy logging.
 *
 * @data:          data bytes (@len bytes), corrected in place on success
 * @len:           number of data bytes (must match the length passed to encode)
 * @parity:        parity bytes (BEAMFS_RS_PARITY)
 * @positions:     optional output, BEAMFS_RS_PARITY/2 = 8 entries; on a
 *                 positive return holds the corrected DATA byte positions
 *                 in [0, len). May be NULL if no entropy logging is needed.
 * @max_positions: capacity of @positions; ignored if @positions is NULL.
 *
 * Returns:
 *   < 0  uncorrectable (-EBADMSG) or invalid input (-EINVAL)
 *   = 0  no errors detected
 *   > 0  number of symbol errors corrected in place (data + parity)
 *
 * Implementation note (Option 2): decode_rs8 cannot both correct in place
 * AND report eras_pos in a single call (its API is mutually exclusive on
 * those modes; see lib/reed_solomon/decode_rs.c). To avoid a second RS
 * decode pass, we snapshot @data before the call and reconstruct the
 * corrected data positions by byte diff afterwards. Parity-side
 * corrections are not reflected in @positions: only data positions are
 * forensically meaningful for the journal, since parity-only flips are
 * an artefact of the codec's internal recovery rather than an indicator
 * of physical media damage at a specific data byte.
 *
 * Stack cost: BEAMFS_SUBBLOCK_DATA = 239 bytes for the snapshot (per call).
 */
int beamfs_rs_decode(u8 *data, size_t len, u8 *parity,
		    int *positions, unsigned int max_positions,
		    const char *who)
{
	u16 par[BEAMFS_RS_PARITY];
	u8 data_orig[BEAMFS_SUBBLOCK_DATA];
	int i, nerr;
	unsigned int n_data_corrected = 0;

	if (!beamfs_rs_ctrl_pcpu)
		return -EINVAL;
	if (len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;
	if (positions && max_positions == 0)
		return -EINVAL;

	for (i = 0; i < BEAMFS_RS_PARITY; i++)
		par[i] = parity[i];

	/* Snapshot data for post-decode position reconstruction. */
	if (positions)
		memcpy(data_orig, data, len);

	/* Mode "apply": corrects data and parity in place. eras_pos = NULL
	 * forces the kernel into the apply branch (decode_rs.c line 315).
	 *
	 * Per-CPU: get_cpu_ptr disables preemption while decode runs in
	 * the CPU-local rs_control. decode_rs8 is pure computation, no
	 * allocs, no sleeps -- preempt-disable is safe.
	 */
	{
		struct rs_control **ctrl_p = get_cpu_ptr(beamfs_rs_ctrl_pcpu);
		struct rs_control *ctrl = *ctrl_p;

		if (!ctrl) {
			put_cpu_ptr(beamfs_rs_ctrl_pcpu);
			return -EINVAL;
		}
		nerr = decode_rs8(ctrl, data, par, len,
				  NULL, 0, NULL, 0, NULL);
		put_cpu_ptr(beamfs_rs_ctrl_pcpu);
	}

	if (nerr < 0) {
		/*
		 * Silent here, on purpose.
		 *
		 * This function knows a length and nothing else -- not the
		 * block, not which of the sixteen subblocks, not what owns
		 * it. It said "RS uncorrectable (len=239) in sweep", which
		 * names the caller and stops exactly where the question
		 * starts. Every caller has results[] and its own context,
		 * and reports from there.
		 */
		return -EBADMSG;
	}

	/* Reconstruct DATA-side corrected positions by byte diff. nerr is
	 * the total count (data + parity); n_data_corrected is the subset
	 * relevant for the entropy estimator.
	 */
	if (positions && nerr > 0) {
		for (i = 0; i < (int)len &&
		     n_data_corrected < max_positions; i++) {
			if (data[i] != data_orig[i])
				positions[n_data_corrected++] = i;
		}
	}

	/* The journal logs n_data_corrected as the entropy sample size,
	 * even though nerr (total) is the value returned upward; callers
	 * that need the entropy-aligned count must derive it from
	 * compute_entropy on the positions buffer they passed in.
	 */
	return nerr;
}

/*
 * beamfs_rs_encode_region -- encode N RS(255,239) shortened subblocks
 * across a region.
 *
 * @data_buf:      base pointer of the data region
 * @data_stride:   distance in bytes between the start of two
 *                 consecutive data subblocks (e.g.
 *                 BEAMFS_SUBBLOCK_TOTAL=255 for the bitmap)
 * @parity_buf:    base pointer of the parity region (may equal
 *                 data_buf + data_len for the interleaved case)
 * @parity_stride: distance between the start of two consecutive
 *                 parity blobs (e.g. BEAMFS_SUBBLOCK_TOTAL=255 for
 *                 the bitmap, BEAMFS_RS_PARITY=16 for contiguous
 *                 parity placement)
 * @data_len:      number of data bytes per subblock (e.g.
 *                 BEAMFS_SUBBLOCK_DATA=239 for a full subblock)
 * @n_subblocks:   number of subblocks to encode
 *
 * Returns 0 on success, negative on the first encode failure.
 * On failure the buffer is in an indeterminate state.
 */
/*
 * Where symbol @i of codeword @sub lives once interleaved.
 *
 * Contiguous, codeword j owns bytes [j*239, (j+1)*239): a nine-byte
 * burst lands entirely in it and takes it past correction, which is
 * what a heavy ion through a die produces and what "subblock N beyond
 * correction" has meant every time it has appeared.
 *
 * Interleaved, the same burst puts one symbol in each of the sixteen
 * codewords. 129 consecutive bytes are needed to lose one, for not a
 * byte of extra parity and no measurable time.
 *
 * The parity stays with its codeword: spreading it too would gain
 * nothing -- a burst in the parity region costs the same either way --
 * and cost a second indirection on every encode.
 */
static inline size_t rs_woven_off(unsigned int sub, size_t i,
				  unsigned int n_subblocks)
{
	return i * n_subblocks + sub;
}

/*
 * Gather one codeword out of an interleaved block.
 *
 * @scratch must hold @data_len bytes. The caller owns it: this runs on
 * the writeback path and an allocation here would be one more thing to
 * fail under the memory pressure that writeback is trying to relieve.
 */
static void rs_gather(const u8 *blk, unsigned int sub, size_t data_len,
		      unsigned int n_subblocks, u8 *scratch)
{
	size_t i;

	for (i = 0; i < data_len; i++)
		scratch[i] = blk[rs_woven_off(sub, i, n_subblocks)];
}

static void rs_scatter(u8 *blk, unsigned int sub, size_t data_len,
		       unsigned int n_subblocks, const u8 *scratch)
{
	size_t i;

	for (i = 0; i < data_len; i++)
		blk[rs_woven_off(sub, i, n_subblocks)] = scratch[i];
}

/*
 * beamfs_rs_encode_woven -- encode a block whose symbols are interleaved.
 *
 * @data_buf:    the interleaved data area, @data_len * @n_subblocks bytes
 * @parity_buf:  where the parity goes, @n_subblocks codewords of it
 * @scratch:     @data_len bytes the caller owns
 *
 * Same contract as beamfs_rs_encode_region, same return values. The
 * difference is where the symbols of a codeword are read from.
 */
int beamfs_rs_encode_woven(u8 *data_buf, u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks)
{
	unsigned int i;
	int ret = 0;
	u8 **sp, *scratch;

	if (!data_buf || !parity_buf || !beamfs_rs_scratch_pcpu)
		return -EINVAL;
	if (data_len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;

	/*
	 * Preemption off for the whole block rather than per codeword:
	 * sixteen encodes of 239 bytes, and taking the pointer sixteen
	 * times costs more than holding it once.
	 */
	sp = get_cpu_ptr(beamfs_rs_scratch_pcpu);
	scratch = *sp;
	if (!scratch) {
		put_cpu_ptr(beamfs_rs_scratch_pcpu);
		return -ENOMEM;
	}

	for (i = 0; i < n_subblocks; i++) {
		rs_gather(data_buf, i, data_len, n_subblocks, scratch);
		ret = beamfs_rs_encode(scratch, data_len,
				       parity_buf + (size_t)i * parity_stride);
		if (ret < 0)
			break;
	}

	put_cpu_ptr(beamfs_rs_scratch_pcpu);
	return ret < 0 ? ret : 0;
}

/*
 * beamfs_rs_decode_woven -- decode one, correcting in place.
 *
 * A codeword that decodes is written back into the block; one that
 * does not is left as it was, so a caller that ignores the result
 * reads what the medium gave rather than a half-corrected mixture.
 */
int beamfs_rs_decode_woven(u8 *data_buf, u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks,
			   int *results, const char *who)
{
	unsigned int i;
	int worst = 0;
	u8 **sp, *scratch;

	if (!data_buf || !parity_buf || !beamfs_rs_scratch_pcpu)
		return -EINVAL;
	if (data_len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;

	sp = get_cpu_ptr(beamfs_rs_scratch_pcpu);
	scratch = *sp;
	if (!scratch) {
		put_cpu_ptr(beamfs_rs_scratch_pcpu);
		return -ENOMEM;
	}

	for (i = 0; i < n_subblocks; i++) {
		int rc;

		rs_gather(data_buf, i, data_len, n_subblocks, scratch);
		rc = beamfs_rs_decode(scratch, data_len,
				      parity_buf + (size_t)i * parity_stride,
				      NULL, 0, who);
		if (results)
			results[i] = rc;
		if (rc < 0) {
			worst = rc;
			continue;
		}
		rs_scatter(data_buf, i, data_len, n_subblocks, scratch);
		if (rc > worst)
			worst = rc;
	}

	put_cpu_ptr(beamfs_rs_scratch_pcpu);
	return worst;
}

/*
 * Encode a 4096-byte block in the grouped layout.
 *
 * The block is its own argument rather than a data pointer and a
 * parity pointer: the two are at fixed offsets within it and passing
 * them apart invites the mismatch that cost an afternoon -- woven
 * functions expecting a compact area, callers handing them an
 * alternating one.
 *
 * Returns 0, or the first encode failure.
 */
int beamfs_rs_encode_block(u8 *block)
{
	if (!block)
		return -EINVAL;

	return beamfs_rs_encode_woven(block + BEAMFS_DATA_WOVEN_OFF,
				      block + BEAMFS_DATA_WOVEN_PARITY_OFF,
				      BEAMFS_RS_PARITY,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS);
}

/*
 * Decode one, correcting in place.
 *
 * @results, when given, holds one entry per codeword: the number of
 * symbols corrected, or negative for one that would not decode.
 *
 * Returns the worst of those, so a caller that wants only "is this
 * block sound" can test the return and a caller that wants to log
 * which codewords suffered has the array.
 */
int beamfs_rs_decode_block(u8 *block, int *results, const char *who)
{
	if (!block)
		return -EINVAL;

	return beamfs_rs_decode_woven(block + BEAMFS_DATA_WOVEN_OFF,
				      block + BEAMFS_DATA_WOVEN_PARITY_OFF,
				      BEAMFS_RS_PARITY,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS,
				      results, who);
}

/*
 * Read @len bytes of user data at @off out of a block.
 *
 * Grouped, the data is contiguous but the symbols of a codeword are
 * not: byte @off of the user's data is at @off within the data area,
 * because interleaving moves symbols within a codeword and not the
 * bytes the user sees. The two are the same array read two ways.
 *
 * This exists so nothing outside edac has to know that.
 */
void beamfs_block_read(const u8 *block, size_t off, size_t len, u8 *out)
{
	memcpy(out, block + BEAMFS_DATA_WOVEN_OFF + off, len);
}

void beamfs_block_write(u8 *block, size_t off, size_t len, const u8 *in)
{
	memcpy(block + BEAMFS_DATA_WOVEN_OFF + off, in, len);
}

int beamfs_rs_encode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks)
{
	unsigned int i;

	if (!data_buf || !parity_buf)
		return -EINVAL;

	/*
	 * A stride of BEAMFS_SUBBLOCK_TOTAL means a 4096-byte block laid
	 * out as sixteen data-plus-parity subblocks: a data block, an
	 * indirect block, a directory block. Those are the ones that sit
	 * across a die and meet bursts, and those are the ones that get
	 * interleaved.
	 *
	 * The superblock's own geometry and rsbench pass other strides
	 * and stay contiguous, which is right -- neither is laid out
	 * that way.
	 *
	 * Keyed on the stride rather than a flag because every caller
	 * already passes it and none would have to change. The cost is
	 * that the layout is implicit: change a stride and the layout
	 * changes with it. Anyone doing that must read this.
	 */
	/*
	 * Not yet: the woven functions take a compact data area and the
	 * callers pass a block whose data and parity already alternate
	 * every 255 bytes. Wiring one to the other without reconciling
	 * the two geometries would gather the wrong symbols and write
	 * noise where a block used to be.
	 *
	 * The layout change belongs with mkfs and fsck, in one step, on
	 * a volume that says so in its feature bits.
	 */

	for (i = 0; i < n_subblocks; i++) {
		u8 *d = data_buf   + (size_t)i * data_stride;
		u8 *p = parity_buf + (size_t)i * parity_stride;
		int rc = beamfs_rs_encode(d, data_len, p);

		if (rc < 0)
			return rc;
	}
	return 0;
}

/*
 * beamfs_rs_decode_region -- decode N RS(255,239) shortened subblocks
 * across a region. Symmetric to beamfs_rs_encode_region.
 *
 * @results:          optional, n_subblocks entries. On exit each
 *                    entry holds the return value of beamfs_rs_decode
 *                    for the corresponding subblock: < 0 uncorrectable,
 *                    = 0 no errors, > 0 number of corrected symbols.
 *                    Pass NULL to skip per-subblock reporting.
 * @positions_buf:    optional, flat array of n_subblocks * positions_stride
 *                    int entries. On a positive results[i], the first
 *                    results[i] entries of positions_buf[i*stride ..] hold
 *                    the corrected DATA positions for subblock i. May be
 *                    NULL (then positions_stride must be 0).
 * @positions_stride: slot count per subblock in positions_buf; should be
 *                    >= BEAMFS_RS_PARITY/2. Ignored if positions_buf NULL.
 *
 * Returns 0 if every subblock decoded successfully, the first
 * negative error otherwise. Decoding does not stop on error: all
 * subblocks are processed, results[] reflects the per-subblock
 * outcome, and the worst negative error is returned.
 */
int beamfs_rs_decode_region(u8 *data_buf, size_t data_stride,
			   u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks,
			   int *results,
			   int *positions_buf,
			   unsigned int positions_stride,
			   const char *who)
{
	unsigned int i;
	int worst = 0;

	if (!data_buf || !parity_buf)
		return -EINVAL;
	if (positions_buf && positions_stride == 0)
		return -EINVAL;
	if (!positions_buf && positions_stride != 0)
		return -EINVAL;

	for (i = 0; i < n_subblocks; i++) {
		u8 *d = data_buf   + (size_t)i * data_stride;
		u8 *p = parity_buf + (size_t)i * parity_stride;
		int *pos = positions_buf
			? positions_buf + (size_t)i * positions_stride
			: NULL;
		int rc = beamfs_rs_decode(d, data_len, p,
					 pos, positions_stride, who);

		if (results)
			results[i] = rc;
		if (rc < 0 && worst >= 0)
			worst = rc;
	}
	return worst;
}

/*
 * beamfs_crc32 - compute CRC32 checksum
 * @buf: data buffer
 * @len: length in bytes
 *
 * Uses the kernel's hardware-accelerated crc32_le (same as ext4/btrfs).
 * Seed 0xFFFFFFFF, final XOR 0xFFFFFFFF (standard CRC-32/ISO-HDLC).
 */
__u32 beamfs_crc32(const void *buf, size_t len)
{
	return crc32_le(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

/*
 * beamfs_crc32_sb -- compute CRC32 over the meaningful regions of the
 *                   superblock, excluding s_crc32 itself and s_pad.
 *
 * Coverage (computed at compile time from struct layout):
 *   region A: [0, offsetof(s_crc32))         -- magic, counters,
 *                                               version, flags
 *   region B: [offsetof(s_uuid), offsetof(s_pad)) -- uuid, label,
 *                                               RS journal,
 *                                               bitmap_blk, features,
 *                                               protection scheme
 * Total coverage: BEAMFS_SB_RS_COVERAGE_BYTES, asserted at build time
 * by BUILD_BUG_ON below.
 *
 * Chained via crc32_le without intermediate XOR. Must match the
 * userspace mkfs.beamfs::crc32_sb() byte-for-byte so that superblocks
 * formatted by mkfs validate at mount time.
 *
 * On-disk format compatibility: superblocks produced by an mkfs whose
 * struct layout differs from this kernel's (e.g. older v2 images
 * predating the v3 feature-field extension) are correctly rejected by
 * the resulting CRC mismatch.
 */
__u32 beamfs_crc32_sb(const struct beamfs_super_block *fsb)
{
	const u8 *base = (const u8 *)fsb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);
	u32 c;

	BUILD_BUG_ON(sizeof_field(struct beamfs_super_block, s_crc32) != 4);
	BUILD_BUG_ON(off_uuid != off_crc32 + 4);
	BUILD_BUG_ON(off_crc32 + (off_pad - off_uuid) !=
		     BEAMFS_SB_RS_COVERAGE_BYTES);

	c = crc32_le(0xFFFFFFFF, base, off_crc32);
	c = crc32_le(c, base + off_uuid, off_pad - off_uuid);
	return c ^ 0xFFFFFFFF;
}
