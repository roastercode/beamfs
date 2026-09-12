/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM beamfs

#if !defined(_BEAMFS_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _BEAMFS_TRACE_H

#include <linux/tracepoint.h>

/*
 * One event per store into the block tree, and one per read of a slot.
 * Filterable in the usual way -- echo 'ino == 60' > filter -- so a run
 * can be narrowed to the inode that leaked without rebuilding anything.
 */
TRACE_EVENT(beamfs_slot_store,
	TP_PROTO(unsigned long ino, u64 parent, u64 slot, u64 old, u64 new,
		 unsigned int level),
	TP_ARGS(ino, parent, slot, old, new, level),
	TP_STRUCT__entry(
		__field(unsigned long, ino)
		__field(u64, parent)
		__field(u64, slot)
		__field(u64, old)
		__field(u64, new)
		__field(unsigned int, level)
	),
	TP_fast_assign(
		__entry->ino = ino;
		__entry->parent = parent;
		__entry->slot = slot;
		__entry->old = old;
		__entry->new = new;
		__entry->level = level;
	),
	TP_printk("ino=%lu parent=%llu slot=%llu %llu->%llu lvl=%u",
		  __entry->ino, __entry->parent, __entry->slot,
		  __entry->old, __entry->new, __entry->level)
);

/*
 * One parity slot written.
 *
 * A region block holds fourteen slots and several inodes' indirect
 * blocks land in the same one. generic/464 leaves slots describing a
 * block that is not the block on the medium -- subblocks 0..6 with
 * data and no parity, 7..15 with parity and no data, two states in one
 * slot -- and the question is which writer left what, and in which
 * order. @nz is how many of the indirect block's pointers were set
 * when the parity was taken, so a slot can be matched to the block it
 * was computed from.
 */
/*
 * An indirect block read on the allocation path.
 *
 * generic/464 has two writers install a pointer into the same slot of
 * the same indirect block, twenty-five seconds apart, and the second
 * one reads it as zero: xfs_io writes 23967 into slot 5 of block 36508
 * at t=2158.33, and a writeback kworker writes 105019 into the same
 * slot at t=2183.22, with the trace showing 0-> both times. The block
 * held the first pointer; the second writer did not see it.
 *
 * @uptodate says whether the buffer came back already valid -- a
 * buffer that had to be read from the medium is one the cache had
 * dropped, and what comes back is whatever was last written out,
 * which need not be what the last writer put in memory.
 *
 * @slotval is what sits in the slot about to be written, so a read
 * that returns zero where a pointer was installed shows up here rather
 * than being inferred from two slot_store events.
 */
TRACE_EVENT(beamfs_ind_read,
	TP_PROTO(unsigned long ino, u64 blk, unsigned int slot, u64 slotval,
		 int uptodate, int fresh),
	TP_ARGS(ino, blk, slot, slotval, uptodate, fresh),
	TP_STRUCT__entry(
		__field(unsigned long, ino)
		__field(u64, blk)
		__field(unsigned int, slot)
		__field(u64, slotval)
		__field(int, uptodate)
		__field(int, fresh)
	),
	TP_fast_assign(
		__entry->ino = ino;
		__entry->blk = blk;
		__entry->slot = slot;
		__entry->slotval = slotval;
		__entry->uptodate = uptodate;
		__entry->fresh = fresh;
	),
	TP_printk("ino=%lu ind=%llu slot=%u val=%llu uptodate=%d fresh=%d",
		  __entry->ino, __entry->blk, __entry->slot,
		  __entry->slotval, __entry->uptodate, __entry->fresh)
);
TRACE_EVENT(beamfs_parity_slot,
	TP_PROTO(unsigned long ino, u64 phys, u64 region, unsigned int slot,
		 unsigned int nz),
	TP_ARGS(ino, phys, region, slot, nz),
	TP_STRUCT__entry(
		__field(unsigned long, ino)
		__field(u64, phys)
		__field(u64, region)
		__field(unsigned int, slot)
		__field(unsigned int, nz)
	),
	TP_fast_assign(
		__entry->ino = ino;
		__entry->phys = phys;
		__entry->region = region;
		__entry->slot = slot;
		__entry->nz = nz;
	),
	TP_printk("ino=%lu ind=%llu region=%llu slot=%u ptrs=%u",
		  __entry->ino, __entry->phys, __entry->region,
		  __entry->slot, __entry->nz)
);
TRACE_EVENT(beamfs_block_alloc,
	TP_PROTO(unsigned long ino, u64 blk, unsigned int level),
	TP_ARGS(ino, blk, level),
	TP_STRUCT__entry(
		__field(unsigned long, ino)
		__field(u64, blk)
		__field(unsigned int, level)
	),
	TP_fast_assign(
		__entry->ino = ino;
		__entry->blk = blk;
		__entry->level = level;
	),
	TP_printk("ino=%lu blk=%llu lvl=%u",
		  __entry->ino, __entry->blk, __entry->level)
);

TRACE_EVENT(beamfs_block_free,
	TP_PROTO(unsigned long ino, u64 blk, unsigned int site),
	TP_ARGS(ino, blk, site),
	TP_STRUCT__entry(
		__field(unsigned long, ino)
		__field(u64, blk)
		__field(unsigned int, site)
	),
	TP_fast_assign(
		__entry->ino = ino;
		__entry->blk = blk;
		__entry->site = site;
	),
	TP_printk("ino=%lu blk=%llu site=%u",
		  __entry->ino, __entry->blk, __entry->site)
);

TRACE_EVENT(beamfs_write_inode,
	TP_PROTO(unsigned long ino, int sync, u64 indirect, int err),
	TP_ARGS(ino, sync, indirect, err),
	TP_STRUCT__entry(
		__field(unsigned long, ino)
		__field(int, sync)
		__field(u64, indirect)
		__field(int, err)
	),
	TP_fast_assign(
		__entry->ino = ino;
		__entry->sync = sync;
		__entry->indirect = indirect;
		__entry->err = err;
	),
	TP_printk("ino=%lu sync=%d i_indirect=%llu err=%d",
		  __entry->ino, __entry->sync, __entry->indirect, __entry->err)
);

TRACE_EVENT(beamfs_inode_dirty,
	TP_PROTO(unsigned long ino, unsigned long state),
	TP_ARGS(ino, state),
	TP_STRUCT__entry(
		__field(unsigned long, ino)
		__field(unsigned long, state)
	),
	TP_fast_assign(
		__entry->ino = ino;
		__entry->state = state;
	),
	TP_printk("ino=%lu state=0x%lx", __entry->ino, __entry->state)
);

#endif /* _BEAMFS_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE beamfs_trace
#include <trace/define_trace.h>
