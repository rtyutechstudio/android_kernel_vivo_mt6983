// SPDX-License-Identifier: GPL-2.0
/*
 * Stock X80 shared SVP/QoS enqueue + dynamic QoS mutation owners.
 * Image 1d3b94,478f78,478c7c. Genuine types/locks/refcounts, unchanged OEM
 * arrays. Pair with matching dequeue, trace, fair/tick/expiry owners.
 */
#include "sched.h"
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <asm/cmpxchg.h>
#include "x80_stock_qos_rq_private.h"
#include "x80_stock_qos_enqueue.h"

static_assert(offsetof(struct rq, clock) == 0xba0);
static_assert(offsetof(struct task_struct, state) == 0x30);
static_assert(offsetof(struct task_struct, on_rq) == 0x80);
static_assert(offsetof(struct task_struct, sched_class) == 0x98);
static_assert(offsetof(struct task_struct, se.avg.util_avg) == 0x270);
static_assert(sizeof(((struct task_struct *)0)->se.avg.util_avg) == 8);
static_assert(offsetof(struct task_struct, android_oem_data1[32]) == 0xe10);
static_assert(offsetof(struct task_struct, android_oem_data1[33]) == 0xe18);
static_assert(offsetof(struct task_struct, android_oem_data1[34]) == 0xe20);
static_assert(0x138 + sizeof(u8) <= X80_TASK_OEM_SIZE);
static_assert(8 + 6 <= 16);

static void x80_enqueue_write64(struct task_struct *task, unsigned int off,
				u64 value)
{
	WRITE_ONCE(*(u64 *)(x80_stock_task_oem_mut(task) + off), value);
}
static u64 *x80_enqueue_util_slot(struct rq *rq, unsigned int slot)
{
	return &rq->android_oem_data1[8 + slot];
}
static void x80_enqueue_mark_deferred(struct task_struct *task)
{
	u8 *address = x80_stock_task_oem_mut(task) + X80_QOS_FLAGS_D6D;
	u8 old;

	do {
		old = smp_load_acquire(address);
	} while (cmpxchg(address, old, (u8)(old | 0x80)) != old);
}

/* Deterministic policy body; the production wrapper supplies live inputs. */
static void x80_enqueue_svp_at(struct rq *rq, struct task_struct *task,
		int raw_qos, u64 now, u8 cpu_qos_enabled, u8 qos_bw_enabled,
		int insertion_enabled, const u32 granularity_ns[6])
{
	struct list_head *node, *head, *cursor;
	u8 *oem = x80_stock_task_oem_mut(task);
	u8 qos = (u8)raw_qos, flags;
	bool overtime = false;
	unsigned int slot;
	u64 start, util;

	lockdep_assert_held(&rq->lock);
	if (READ_ONCE(task->on_rq) != TASK_ON_RQ_MIGRATING &&
	    !(READ_ONCE(oem[0x137]) & 0x01)) {
		x80_enqueue_write64(task, 0x108, now);       /* e18 */
		WRITE_ONCE(oem[0x138], 0);                 /* e48 */
		x80_enqueue_write64(task, 0x110,
				    READ_ONCE(task->se.sum_exec_runtime));
		if (qos) {
			BUG_ON((unsigned int)(qos - 1) >= 6);
			x80_enqueue_write64(task, 0x100,
					    now + READ_ONCE(granularity_ns[qos - 1]));
		}
	}
	node = x80_stock_task_shared_node(task);
	if (smp_load_acquire(&node->next) != node)
		return;
	x80_enqueue_write64(task, 0xa0, READ_ONCE(rq->clock)); /* db0 */
	if (qos_bw_enabled) {
		start = x80_stock_task_read_u64(task, 0x108);
		overtime = (s64)(start - now + 10000000000ULL) < 0;
	}
	flags = READ_ONCE(oem[X80_QOS_FLAGS_D6D]);
	if (!(flags & 0x40) &&
	    (overtime || !qos || !cpu_qos_enabled) &&
	    !x80_stock_task_boost_acquire(task)) {
		if (flags & 0x01)
			x80_enqueue_mark_deferred(task);
		return;
	}
	head = x80_stock_rq_shared_head(rq);
	list_for_each(cursor, head)
		if (cursor == node)
			return;
	if (insertion_enabled) {
		WRITE_ONCE(*x80_stock_rq_shared_count(rq),
			   (u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) + 1));
		list_add_tail(node, head);
		if (cpu_qos_enabled && qos) {
			WRITE_ONCE(oem[X80_QOS_LAST_ACCOUNTED], qos);
			slot = qos - 1;
			BUG_ON(slot >= 6);
			util = smp_load_acquire(&task->se.avg.util_avg);
			WRITE_ONCE(*x80_enqueue_util_slot(rq, slot),
				   READ_ONCE(*x80_enqueue_util_slot(rq, slot)) + util);
			x80_enqueue_write64(task, 0x100,
					    now + READ_ONCE(granularity_ns[slot]));
		}
		get_task_struct(task);
	}
	/* Stock also traces the admitted-but-disabled insertion case. */
	x80_stock_trace_svp_queue_op(task, "svp_enqueue");
}

void x80_stock_enqueue_svp_task(struct rq *rq, struct task_struct *task)
{
	int qos = get_task_qos(task);
	u64 now = sched_clock();

	x80_enqueue_svp_at(rq, task, qos, now,
		READ_ONCE(rsc_cpu_qos_enable), (u8)READ_ONCE(rsc_qos_bw_enable),
		READ_ONCE(dynamic_svp_enqueue_enable), rsc_max_qos_granularity_ns);
}

/*
 * Dynamic QoS is NOT the scheduler enqueue wrapper: it extends deadline even
 * for already-linked/sleeping tasks and has exact 0/1/2/3/-EINVAL outcomes.
 * Its callers already establish a real donated QoS byte; do not guess one.
 */
static int x80_dynamic_qos_enqueue_at(struct rq *rq, struct task_struct *task,
		int qos, u64 deadline_now, int insertion_enabled,
		const u32 granularity_ns[6])
{
	struct list_head *node, *head, *cursor;
	u8 *oem = x80_stock_task_oem_mut(task);
	unsigned int slot;
	u64 deadline, util, enqueue_now;

	lockdep_assert_held(&rq->lock);
	if (task->sched_class != &fair_sched_class)
		return -EINVAL;
	slot = (u32)qos - 1;
	BUG_ON(slot >= 6);
	deadline = deadline_now + READ_ONCE(granularity_ns[slot]);
	if (deadline > x80_stock_task_read_u64(task, 0x100))
		x80_enqueue_write64(task, 0x100, deadline);
	node = x80_stock_task_shared_node(task);
	if (smp_load_acquire(&node->next) != node)
		return READ_ONCE(task->state) ? 2 : 1;
	if (READ_ONCE(task->on_rq) != TASK_ON_RQ_QUEUED)
		return 3;
	head = x80_stock_rq_shared_head(rq);
	list_for_each(cursor, head)
		if (cursor == node)
			return 0;
	if (!insertion_enabled)
		return 0;
	get_task_struct(task);
	list_add_tail(node, head);
	util = smp_load_acquire(&task->se.avg.util_avg);
	WRITE_ONCE(*x80_enqueue_util_slot(rq, slot),
		   READ_ONCE(*x80_enqueue_util_slot(rq, slot)) + util);
	WRITE_ONCE(oem[X80_QOS_LAST_ACCOUNTED], (u8)qos);
	enqueue_now = sched_clock(); /* Stock independently samples a second time. */
	x80_enqueue_write64(task, 0x110, READ_ONCE(task->se.sum_exec_runtime));
	x80_enqueue_write64(task, 0x108, enqueue_now);
	WRITE_ONCE(oem[0x138], 0);
	WRITE_ONCE(*x80_stock_rq_shared_count(rq),
		   (u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) + 1));
	return 0;
}

int x80_stock_dynamic_qos_enqueue(struct task_struct *task, int qos)
{
	struct rq_flags rf;
	struct rq *rq = task_rq_lock(task, &rf);
	int result;
	u64 now;

	/* Read inside the original stable-rq/pi-lock critical section. */
	now = sched_clock();
	result = x80_dynamic_qos_enqueue_at(rq, task, qos, now,
		READ_ONCE(dynamic_svp_enqueue_enable), rsc_max_qos_granularity_ns);
	task_rq_unlock(rq, task, &rf);
	return result;
}

static void x80_dynamic_qos_dequeue_locked(struct rq *rq,
					 struct task_struct *task)
{
	struct list_head *node = x80_stock_task_shared_node(task);
	struct list_head *head = x80_stock_rq_shared_head(rq), *cursor;
	u8 *oem = x80_stock_task_oem_mut(task);
	unsigned int slot;
	u64 *aggregate, util, value;
	bool found = false;

	lockdep_assert_held(&rq->lock);
	list_for_each(cursor, head) {
		if (cursor == node) {
			found = true;
			break;
		}
	}
	if (!found)
		return;
	slot = (u32)READ_ONCE(oem[X80_QOS_LAST_ACCOUNTED]) - 1;
	if (slot < 6) {
		aggregate = x80_enqueue_util_slot(rq, slot);
		value = READ_ONCE(*aggregate);
		util = smp_load_acquire(&task->se.avg.util_avg);
		if (value < util)
			value = 0;
		else {
			util = smp_load_acquire(&task->se.avg.util_avg);
			value = READ_ONCE(*aggregate) - util;
		}
		WRITE_ONCE(*aggregate, value);
		WRITE_ONCE(oem[X80_QOS_LAST_ACCOUNTED], 0);
	} else if (READ_ONCE(rsc_qos_debug) & 0x20) {
		printk(KERN_WARNING "^RSC QoS%s QoS dequeue non-qos util\n",
		       "dequeue_qos_util");
	}
	list_del_init(node);
	WRITE_ONCE(*x80_stock_rq_shared_count(rq),
		   (u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) - 1));
	put_task_struct(task);
}

void x80_stock_dynamic_qos_dequeue(struct task_struct *task)
{
	struct rq_flags rf;
	struct rq *rq = task_rq_lock(task, &rf);

	x80_dynamic_qos_dequeue_locked(rq, task);
	task_rq_unlock(rq, task, &rf);
}
