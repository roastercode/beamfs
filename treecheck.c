// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs tree checker -- catch a lost pointer where it is lost.
 *
 * generic/464 reports the damage through fsck, twenty seconds after the
 * fact, as a count of blocks the bitmap calls used and no inode
 * reaches. Working back from that count means reading a million trace
 * lines and guessing which of them mattered, which is how two weeks
 * went by with three patches that each measured worse than the code
 * they replaced.
 *
 * The way out of chasing an intermittent defect is to stop chasing it:
 * check the invariant at every write, and the first violation arrives
 * as a stack trace naming the task, the block and the slot, instead of
 * a number in a log.
 *
 * The invariant: a slot that holds a pointer keeps it until the block
 * it names is freed. Every store into an indirect block is recorded
 * here; every subsequent store to the same slot is checked against the
 * record. A store that finds zero where a live pointer was recorded is
 * the defect, and it is reported at that instant.
 *
 * Cost: one hash lookup and one 16-byte entry per live pointer. Off
 * unless CONFIG_BEAMFS_DEBUG_TREE, and the calls compile away entirely
 * when it is off.
 */

#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "beamfs.h"

#ifdef CONFIG_BEAMFS_DEBUG_TREE

#define BEAMFS_TC_BITS 14

struct beamfs_tc_entry {
	struct hlist_node node;
	u64 parent;
	u32 slot;
	u64 child;
	unsigned long ino;
};

/*
 * Keyed on (parent, slot) rather than on the child: the question asked
 * at every store is "what did this slot hold", and a child that has
 * been freed and handed out again would collide under the other key.
 */
static inline u32 beamfs_tc_key(u64 parent, u32 slot)
{
	return hash_64(parent * BEAMFS_INDIRECT_PTRS + slot, BEAMFS_TC_BITS);
}

void beamfs_tc_init(struct beamfs_sb_info *sbi)
{
	hash_init(sbi->s_tc);
	spin_lock_init(&sbi->s_tc_lock);
	sbi->s_tc_entries = 0;
	sbi->s_tc_violations = 0;
}

void beamfs_tc_exit(struct beamfs_sb_info *sbi)
{
	struct beamfs_tc_entry *e;
	struct hlist_node *tmp;
	unsigned int bkt;

	spin_lock(&sbi->s_tc_lock);
	hash_for_each_safe(sbi->s_tc, bkt, tmp, e, node) {
		hash_del(&e->node);
		kfree(e);
	}
	spin_unlock(&sbi->s_tc_lock);

	if (sbi->s_tc_violations)
		pr_err("beamfs/treecheck: %u lost pointer(s) over the life of this mount\n",
		       sbi->s_tc_violations);
	else
		pr_info("beamfs/treecheck: no lost pointer seen\n");
}

/*
 * Record a store, and check what the slot held before it.
 *
 * @old is what the caller read out of the slot. If a live pointer was
 * recorded for this slot and @old is zero, that pointer vanished from
 * the buffer between the two stores with nothing freeing it -- which is
 * the defect, reported here with the task and the stack that found it.
 */
void beamfs_tc_store(struct super_block *sb, unsigned long ino, u64 parent,
		     u32 slot, u64 old, u64 child)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e, *found = NULL;
	u32 key = beamfs_tc_key(parent, slot);

	spin_lock(&sbi->s_tc_lock);

	hash_for_each_possible(sbi->s_tc, e, node, key) {
		if (e->parent == parent && e->slot == slot) {
			found = e;
			break;
		}
	}

	if (found && found->child && old == 0) {
		sbi->s_tc_violations++;
		spin_unlock(&sbi->s_tc_lock);
		pr_err("beamfs/treecheck: LOST POINTER parent=%llu slot=%u held %llu (ino=%lu), read as 0, now %llu (ino=%lu)\n",
		       (unsigned long long)parent, slot,
		       (unsigned long long)found->child, found->ino,
		       (unsigned long long)child, ino);
		WARN_ONCE(1, "beamfs: indirect slot lost its pointer\n");
		spin_lock(&sbi->s_tc_lock);
		found->child = child;
		found->ino = ino;
		spin_unlock(&sbi->s_tc_lock);
		return;
	}

	if (found) {
		found->child = child;
		found->ino = ino;
		spin_unlock(&sbi->s_tc_lock);
		return;
	}

	spin_unlock(&sbi->s_tc_lock);

	/* GFP_NOFS: this runs inside writeback. */
	e = kmalloc_obj(*e, GFP_NOFS);
	if (!e)
		return;   /* a checker that fails allocation checks less, not wrongly */
	e->parent = parent;
	e->slot = slot;
	e->child = child;
	e->ino = ino;

	spin_lock(&sbi->s_tc_lock);
	hash_add(sbi->s_tc, &e->node, key);
	sbi->s_tc_entries++;
	spin_unlock(&sbi->s_tc_lock);
}

/*
 * A freed block's pointer is allowed to disappear.
 *
 * Called from beamfs_free_block. Every slot naming this block is
 * forgotten, so a later store into that slot is not a violation. Without
 * this the checker would report every ordinary truncate.
 */
void beamfs_tc_forget_child(struct super_block *sb, u64 child)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e;
	struct hlist_node *tmp;
	unsigned int bkt;

	spin_lock(&sbi->s_tc_lock);
	hash_for_each_safe(sbi->s_tc, bkt, tmp, e, node) {
		if (e->child == child) {
			hash_del(&e->node);
			sbi->s_tc_entries--;
			kfree(e);
		}
	}
	spin_unlock(&sbi->s_tc_lock);
}

/*
 * A freed parent takes its slots with it.
 *
 * The block is about to be handed to somebody else, who will zero it
 * and store into the same slots. Those stores are not violations.
 */
void beamfs_tc_forget_parent(struct super_block *sb, u64 parent)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	struct beamfs_tc_entry *e;
	struct hlist_node *tmp;
	unsigned int bkt;

	spin_lock(&sbi->s_tc_lock);
	hash_for_each_safe(sbi->s_tc, bkt, tmp, e, node) {
		if (e->parent == parent) {
			hash_del(&e->node);
			sbi->s_tc_entries--;
			kfree(e);
		}
	}
	spin_unlock(&sbi->s_tc_lock);
}

#endif /* CONFIG_BEAMFS_DEBUG_TREE */
