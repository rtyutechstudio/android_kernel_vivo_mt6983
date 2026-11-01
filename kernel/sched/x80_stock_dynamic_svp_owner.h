/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_DYNAMIC_SVP_OWNER_H
#define _X80_STOCK_DYNAMIC_SVP_OWNER_H
/* Include after genuine kernel/sched/sched.h. Source candidate, not wired. */
#include "x80_stock_qos_enqueue_owner.h"
#include <linux/errno.h>
#include <linux/atomic.h>

/* Real writers: Image+46f88c/46f8a0, +46f988/46f990,
 * +46f9d4/46f9e4 and +46fa08/46fa14 respectively. */
extern unsigned long dynamic_svp_count[7];
extern unsigned int dynamic_svp_over_count[7];
extern int dynamic_svp_max_depth;
extern unsigned int dynamic_svp_depth_count[16];
extern unsigned int svp_max_dynamic_granularity_ns;
extern unsigned int svp_max_dynamic_granularity_ns_for_binder;

static_assert(offsetof(struct task_struct, state) == 0x30);
static_assert(offsetof(struct task_struct, sched_class) == 0x98);
static_assert(sizeof(atomic64_t) == sizeof(u64));

enum x80_dynamic_svp_offsets {
	X80_SVP_DEPTH = 0x5e, /* absolute d6e byte */
	X80_SVP_DYNAMIC_EXPIRES = 0xb0, /* absolute dc0 ns */
};

static inline atomic64_t *x80_stock_dynamic_svp_refs(struct task_struct *p)
{
	return (atomic64_t *)(x80_stock_task_oem_mut(p) + X80_QOS_BOOST_DB8);
}

/* +46f868..46fcc8. RQ lock pins CPU against migration and serializes list,
 * count, util, and ref changes with enqueue/dequeue scheduler owners.
 * Lifetime supplied by caller (the shared-list ref is an ADDITIONAL ref).
 * Deliberately does not call scheduler enqueue: dynamic boost has different
 * clocks, depth, QoS deadline, return values and admission predicates.
 * Trace/print-only branches are not reproduced here. */
static inline int x80_stock_dynamic_svp_enqueue_locked(struct rq *rq,
		struct task_struct *p, int reason, int depth, u64 (*clock_ns)(void))
{
	u8 *oem = x80_stock_task_oem_mut(p);
	struct list_head *node = x80_stock_task_shared_node(p);
	struct list_head *head = x80_stock_rq_shared_head(rq), *cursor;
	u64 refs, expiry, value, util, now;
	u32 next_depth = (u32)depth + 1;
	unsigned int shift, slot;
	int qos;

	lockdep_assert_held(&rq->lock);
	if (p->sched_class != &fair_sched_class)
		return -ERANGE;
	/* Stock signed reason>6 returns EINVAL before rq lock, negative traps. */
	BUG_ON((unsigned int)reason > 6);
	shift = (unsigned int)reason * 4;
	WRITE_ONCE(dynamic_svp_count[reason],
		READ_ONCE(dynamic_svp_count[reason]) + 1);
	refs = x80_stock_task_boost_acquire(p);
	if (((refs >> shift) & 15) == 15) {
		WRITE_ONCE(dynamic_svp_over_count[reason],
			READ_ONCE(dynamic_svp_over_count[reason]) + 1);
	} else {
		/* STADD/LDXR+STXR relaxed; do not replace with whole-word store. */
		atomic64_add(1ULL << shift, x80_stock_dynamic_svp_refs(p));
		refs = x80_stock_task_boost_acquire(p);
		now = clock_ns();
		expiry = now + READ_ONCE(svp_max_dynamic_granularity_ns_for_binder);
		if (reason && !(refs & 15))
			expiry = now + READ_ONCE(svp_max_dynamic_granularity_ns);
		value = x80_stock_task_read_u64(p, X80_SVP_DYNAMIC_EXPIRES);
		if (!value || (s64)(value - expiry) < 0)
			x80_stock_enqueue_store_u64(p, X80_SVP_DYNAMIC_EXPIRES, expiry);
	}
	if ((s32)next_depth > READ_ONCE(dynamic_svp_max_depth))
		WRITE_ONCE(dynamic_svp_max_depth, (s32)next_depth);
	if ((s32)next_depth > READ_ONCE(oem[X80_SVP_DEPTH]))
		WRITE_ONCE(oem[X80_SVP_DEPTH], (u8)next_depth);
	/* Stock traps for negative next_depth on the indexed path; >14 =>15. */
	if ((s32)next_depth > 14)
		slot = 15;
	else {
		BUG_ON(next_depth > 16);
		slot = next_depth;
	}
	WRITE_ONCE(dynamic_svp_depth_count[slot],
		READ_ONCE(dynamic_svp_depth_count[slot]) + 1);
	now = clock_ns();
	value = now + READ_ONCE(rsc_max_qos_granularity_ns[5]);
	if (value > x80_stock_task_read_u64(p, X80_QOS_DEADLINE))
		x80_stock_enqueue_store_u64(p, X80_QOS_DEADLINE, value);

	if (smp_load_acquire(&node->next) != node)
		return READ_ONCE(p->state) ? 2 : 1;
	if (READ_ONCE(p->on_rq) != TASK_ON_RQ_QUEUED)
		return 3;
	list_for_each(cursor, head)
		if (cursor == node)
			return 0;
	if (!READ_ONCE(dynamic_svp_enqueue_enable))
		return 0;
	get_task_struct(p);
	list_add_tail(node, head);
	if (READ_ONCE(rsc_cpu_qos_enable)) {
		qos = (u8)get_task_qos(p);
		if (qos >= 1 && qos <= 6) {
			util = smp_load_acquire(&p->se.avg.util_avg);
			WRITE_ONCE(*x80_stock_rq_qos_util_slot(rq, qos - 1),
				READ_ONCE(*x80_stock_rq_qos_util_slot(rq, qos - 1)) + util);
			WRITE_ONCE(oem[X80_QOS_LAST_ACCOUNTED], qos);
		}
	}
	now = clock_ns();
	x80_stock_enqueue_store_u64(p, X80_QOS_ENQUEUE_RUNTIME,
		READ_ONCE(p->se.sum_exec_runtime));
	x80_stock_enqueue_store_u64(p, X80_QOS_ENQUEUE_CLOCK, now);
	WRITE_ONCE(oem[X80_QOS_ACCOUNT_RESET], 0);
	WRITE_ONCE(*x80_stock_rq_shared_count(rq),
		(u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) + 1));
	return 0;
}

/* +470b30..470f10. Do NOT call dequeue_svp_task: remaining nibbles and
 * enabled nonzero QoS retain the SAME node/reference and util accounting. */
static inline void x80_stock_dynamic_svp_dequeue_locked(struct rq *rq,
		struct task_struct *p, int reason)
{
	u8 *oem = x80_stock_task_oem_mut(p);
	struct list_head *head = x80_stock_rq_shared_head(rq);
	struct list_head *node = x80_stock_task_shared_node(p), *cursor;
	u64 refs = x80_stock_task_boost_acquire(p), util, total;
	unsigned int shift = ((u32)reason << 2) & 63, slot;
	bool found = false;

	lockdep_assert_held(&rq->lock);
	/* Stock zero entry clears only depth and returns, NOT list or expiry. */
	if (!refs) {
		WRITE_ONCE(oem[X80_SVP_DEPTH], 0);
		return;
	}
	if ((x80_stock_task_boost_acquire(p) >> shift) & 15)
		atomic64_sub(1ULL << shift, x80_stock_dynamic_svp_refs(p));
	if (x80_stock_task_boost_acquire(p))
		return;
	WRITE_ONCE(oem[X80_SVP_DEPTH], 0);
	x80_stock_enqueue_store_u64(p, X80_SVP_DYNAMIC_EXPIRES, 0);
	list_for_each(cursor, head)
		if (cursor == node) {
			found = true;
			break;
		}
	if (!found)
		return;
	if (READ_ONCE(rsc_cpu_qos_enable) && get_task_qos(p))
		return;
	slot = (u32)READ_ONCE(oem[X80_QOS_LAST_ACCOUNTED]) - 1;
	if (slot < 6) {
		total = READ_ONCE(*x80_stock_rq_qos_util_slot(rq, slot));
		util = smp_load_acquire(&p->se.avg.util_avg);
		if (total < util)
			total = 0;
		else
			total = READ_ONCE(*x80_stock_rq_qos_util_slot(rq, slot)) -
				smp_load_acquire(&p->se.avg.util_avg);
		WRITE_ONCE(*x80_stock_rq_qos_util_slot(rq, slot), total);
		WRITE_ONCE(oem[X80_QOS_LAST_ACCOUNTED], 0);
	}
	list_del_init(node);
	WRITE_ONCE(*x80_stock_rq_shared_count(rq),
		(u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) - 1));
	put_task_struct(p);
}

/* Private recovered names: exact public prototypes await authoritative
 * owner headers. requester and dependency are trace-only stock inputs. */
static inline int x80_stock_dynamic_svp_enqueue_internal(struct task_struct *p,
		struct task_struct *requester, int reason, int depth,
		const char *dependency)
{
	struct rq_flags rf;
	struct rq *rq;
	int result;

	if (reason > 6)
		return -EINVAL;
	rq = task_rq_lock(p, &rf);
	result = x80_stock_dynamic_svp_enqueue_locked(rq, p, reason, depth,
		sched_clock);
	task_rq_unlock(rq, p, &rf);
	return result;
}

static inline void x80_stock___dynamic_svp_dequeue(struct task_struct *p,
		int reason)
{
	struct rq_flags rf;
	struct rq *rq = task_rq_lock(p, &rf);

	x80_stock_dynamic_svp_dequeue_locked(rq, p, reason);
	task_rq_unlock(rq, p, &rf);
}
#endif
