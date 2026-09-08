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
