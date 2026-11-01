/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_DEQUEUE_OWNER_H
#define _X80_STOCK_QOS_DEQUEUE_OWNER_H
/* Source candidate only. Include AFTER genuine sched.h and enqueue owner.
 * Caller holds rq lock; never add another rq lock here. */
#include <linux/cred.h>
#include <linux/printk.h>
#include "x80_stock_qos_trace_owner.h"
#include "x80_stock_qos_enqueue_owner.h"

extern u32 dynamic_svp_timeout_deq_count[7];
extern atomic_t rsc_svp_task_cnt;
extern u32 rsc_qos_debug;

static inline void x80_stock_svp_clear_deferred(struct task_struct *task)
{
	u8 *p = x80_stock_task_oem_mut(task) + X80_QOS_FLAGS_D6D;
	u8 old;
	if (!(READ_ONCE(*p) & 0x80))
		return;
	do {
		old = smp_load_acquire(p);
	} while (cmpxchg(p, old, old & 0x7f) != old);
}

/* Return the stock LOCAL boost value: expiry can retain bits above nibble6
 * locally even though the stored boost is cleared unconditionally. */
static inline u64 x80_stock_svp_expire_dequeue(struct task_struct *task,
		u64 boost, u64 now)
{
	u8 *oem = x80_stock_task_oem_mut(task);
	unsigned int i;
	u64 mask;

	if (!boost || !((x80_stock_task_read_u64(task, 0xb0) - now) >> 63))
		return boost;
	WRITE_ONCE(*(u16 *)(oem + 0xe0), 0); /* df0 */
	for (i = 0; i < 7; i++) {
		/* dd0..de8 clear ON REACHING each stage, before nibble test. */
		if (i >= 1 && i <= 4)
			x80_stock_enqueue_store_u64(task, 0xb8 + i * 8, 0);
		mask = 0xfULL << (i * 4);
		if (boost & mask) {
			WRITE_ONCE(dynamic_svp_timeout_deq_count[i],
				   READ_ONCE(dynamic_svp_timeout_deq_count[i]) + 1);
			boost &= ~mask;
		}
		if (!boost)
			break;
	}
	WRITE_ONCE(oem[X80_QOS_SENTINEL_DF2], 0xff);
	x80_stock_enqueue_store_u64(task, X80_QOS_BOOST_DB8, 0);
	WRITE_ONCE(oem[0x5e], 0); /* d6e depth */
	return boost;
}

static inline void x80_stock_svp_dequeue_not_found(struct rq *rq,
		struct task_struct *task)
{
	unsigned int uid;
	rcu_read_lock();
	uid = __kuid_val(__task_cred(task)->uid);
	rcu_read_unlock();
	printk_deferred("RSC [WARNING] vmigrate svp not found in dequeue_svp_task. uid %5d %16s %5d(%16s %5d) father: %16s %5d svpcnt: %d - %u cpuset: %*pbl cpu%d\n",
		uid, task->comm, task->pid, task->group_leader->comm,
		task->group_leader->pid, task->real_parent->comm,
		task->real_parent->pid, atomic_read(&rsc_svp_task_cnt),
		READ_ONCE(*x80_stock_rq_shared_count(rq)), nr_cpu_ids,
		&task->cpus_mask, rq->cpu);
}

static inline void x80_stock_qos_subtract_util(struct task_struct *task,
		u64 *slot)
{
	u64 result;
	if (READ_ONCE(*slot) < smp_load_acquire(&task->se.avg.util_avg))
		result = 0;
	else {
		/* Required acquire SECOND load, THEN slot SECOND load. */
		u64 util = smp_load_acquire(&task->se.avg.util_avg);
		result = READ_ONCE(*slot) - util;
	}
	WRITE_ONCE(*slot, result);
}

/* Clock is deliberately obtained by wrapper before rq/task NULL checks. */
static inline void x80_stock_dequeue_svp_task_at(struct rq *rq,
		struct task_struct *task, u64 now)
{
	struct list_head *node, *head, *cursor, *next, *prev;
	u8 *oem;
	u64 boost, *slot;
	unsigned int index;

	if (!rq || !task)
		return;
	lockdep_assert_held(&rq->lock);
	oem = x80_stock_task_oem_mut(task);
	x80_stock_enqueue_store_u64(task, X80_SVP_RQ_CLOCK, 0);
	node = x80_stock_task_shared_node(task);
	if (smp_load_acquire(&node->next) == node) {
		x80_stock_svp_clear_deferred(task);
		return;
	}
	boost = x80_stock_task_boost_acquire(task);
	boost = x80_stock_svp_expire_dequeue(task, boost, now);
	head = x80_stock_rq_shared_head(rq);
	list_for_each(cursor, head)
		if (cursor == node)
			goto found;
	x80_stock_svp_dequeue_not_found(rq, task);
	return;
found:
	WRITE_ONCE(*x80_stock_rq_shared_count(rq),
		   (u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) - 1));
	x80_stock_svp_clear_deferred(task);
	/* +80 is on_rq, NOT policy. Preserve the old audit correction. */
	if ((boost & 0xf000000ULL) && !READ_ONCE(task->on_rq)) {
		x80_stock_enqueue_store_u64(task, X80_QOS_BOOST_DB8,
					   boost & ~0xf000000ULL);
		x80_stock_enqueue_store_u64(task, 0xb0, now);
		WRITE_ONCE(oem[0x5e], 0);
	}
	/* Stock calls validation but does NOT branch on its boolean result.
	 * list_del_init() would incorrectly suppress unlink on invalid result. */
	(void)__list_del_entry_valid(node);
	next = READ_ONCE(node->next);
	prev = READ_ONCE(node->prev);
	WRITE_ONCE(next->prev, prev);
	WRITE_ONCE(prev->next, next);
	WRITE_ONCE(node->next, node);
	index = (unsigned int)READ_ONCE(oem[X80_QOS_LAST_ACCOUNTED]) - 1;
	WRITE_ONCE(node->prev, node);
	if (index < 6) {
		slot = x80_stock_rq_qos_util_slot(rq, index);
		x80_stock_qos_subtract_util(task, slot);
		WRITE_ONCE(oem[X80_QOS_LAST_ACCOUNTED], 0);
	} else if (READ_ONCE(rsc_qos_debug) & 0x20) {
		printk(KERN_WARNING "^RSC QoS%s QoS dequeue non-qos util\n",
		       "dequeue_qos_util");
	}
	put_task_struct(task);
	x80_stock_trace_svp_queue_op(task, "svp_dequeue");
}

static inline void x80_stock_dequeue_svp_task(struct rq *rq,
		struct task_struct *task)
{
	u64 now = sched_clock();
	x80_stock_dequeue_svp_task_at(rq, task, now);
}
#endif
