// SPDX-License-Identifier: GPL-2.0
/* Finite stock pick_first_svp_task/pick_svp_task and BW-locked caller. */
#include "sched.h"
#include "x80_stock_qos_bandwidth.h"
#include <linux/sched/task.h>

#ifdef CONFIG_VIVO_X80_UI_BOOST
/* Optional legacy UI-boost subsystem owns these two real identities/state. */
extern struct task_struct *rsc_launcher_gc;
extern int rsc_in_critical_path;
extern u32 rsc_max_qos_special_enqueue_granularity_ns;
#endif
extern int svp_min_sched_delay_granularity;
extern atomic_t rsc_svp_task_cnt;
extern u32 rsc_svp_preempt_pickup_count;
extern int in_cts_atracetest;
/* Parent provides one real trace wrapper shared with enqueue/dequeue. */
void x80_stock_trace_svp_queue_op(struct task_struct *task, const char *message);

static struct task_struct *x80_stock_qos_node_task(struct list_head *node)
{
	return (struct task_struct *)((u8 *)node -
		X80_TASK_OEM_BASE - X80_QOS_SHARED_NODE);
}

static void x80_stock_qos_match(struct sched_entity **a, struct sched_entity **b)
{
	while ((*a)->cfs_rq != (*b)->cfs_rq) {
		int ad = (*a)->depth;
		int bd = (*b)->depth;

		if (ad <= bd)
			*b = (*b)->parent;
		if (ad >= bd)
			*a = (*a)->parent;
	}
}

static void x80_stock_qos_clear_pick_flag(struct task_struct *task)
{
	u8 *flag = x80_stock_task_oem_mut(task) + X80_QOS_FLAGS_D6D;
	u8 old;

	if (!(READ_ONCE(*flag) & 0x40))
		return;
	do {
		old = smp_load_acquire(flag);
	} while (cmpxchg(flag, old, old & ~0x40) != old);
}

static void x80_stock_qos_unlink_pick(struct task_struct *task)
{
	struct list_head *node = x80_stock_task_shared_node(task);
	struct list_head *next, *prev;

	(void)__list_del_entry_valid(node);
	next = READ_ONCE(node->next);
	prev = READ_ONCE(node->prev);
	WRITE_ONCE(next->prev, prev);
	WRITE_ONCE(prev->next, next);
	WRITE_ONCE(node->next, node);
	WRITE_ONCE(node->prev, node);
}

static void x80_stock_qos_pick_subtract_util(struct rq *rq,
					 struct task_struct *task)
{
	unsigned int index = (unsigned int)x80_stock_task_read_u8(task,
						X80_QOS_LAST_ACCOUNTED) - 1;
	u64 *slot, value;

	if (index >= 6) {
		if (READ_ONCE(rsc_qos_debug) & 0x20)
			pr_warn("^RSC QoS%s QoS dequeue non-qos util\n", "dequeue_qos_util");
		return;
	}
	slot = &rq->android_oem_data1[8 + index];
	if (READ_ONCE(*slot) < smp_load_acquire(&task->se.avg.util_avg))
		value = 0;
	else {
		u64 util = smp_load_acquire(&task->se.avg.util_avg);

		value = READ_ONCE(*slot) - util;
	}
	WRITE_ONCE(*slot, value);
	WRITE_ONCE(x80_stock_task_oem_mut(task)[X80_QOS_LAST_ACCOUNTED], 0);
}

struct task_struct *x80_stock_pick_first_svp_task(struct rq *rq)
{
	struct list_head *head = x80_stock_rq_shared_head(rq), *node, *next;
	struct task_struct *best, *task;
	struct sched_entity *a, *b;
	bool cpu_qos = READ_ONCE(rsc_cpu_qos_enable);
	bool bandwidth = READ_ONCE(rsc_qos_bw_enable);
	bool second_pass = false;
	unsigned int rounds = 0, index;
	int qos, best_qos;
	u64 runtime, elapsed;

	lockdep_assert_held(&rq->lock);
	/* The bound is stock rq->cfs.nr_running+2, checked before each pass. */
	for (;;) {
		if (rounds++ > READ_ONCE(rq->cfs.nr_running) + 2)
			return NULL;
		best = NULL;
		best_qos = 0;
		list_for_each_safe(node, next, head) {
			task = x80_stock_qos_node_task(node);
			if (smp_load_acquire(&task->cpu) != cpu_of(rq)) {
				/* Corrective difference: stock forgets old-rq count and
				 * list usage reference here. The node is linked under
				 * THIS rq lock so its ownership cannot migrate while
				 * unlinking; release exactly that list reference.
				 */
				printk_deferred("RSC [WARNING] svp CPU mismatch %s %d cpu%d rq%d\n",
					task->comm, smp_load_acquire(&task->cpu),
					task->pid, cpu_of(rq));
				x80_stock_qos_unlink_pick(task);
				x80_stock_qos_pick_subtract_util(rq, task);
				WRITE_ONCE(*x80_stock_rq_shared_count(rq),
					(u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) - 1));
				x80_stock_trace_svp_queue_op(task, "svp_pick_dequeue");
				put_task_struct(task);
				continue;
			}
			if (cfs_rq_throttled(task->se.cfs_rq))
				continue;
			if (cpu_qos && bandwidth && !second_pass &&
			    x80_stock_task_read_u8(task, 0x138))
				continue;
			qos = get_task_qos(task);
			if (!best)
				goto replace;
			if (cpu_qos) {
				if (qos > best_qos)
					goto replace;
				if (qos != best_qos)
					continue;
				a = &task->se;
				b = &best->se;
				x80_stock_qos_match(&a, &b);
				if ((s64)(a->vruntime - b->vruntime) >= 0)
					continue;
			} else if ((s64)(task->se.vruntime - best->se.vruntime) >= 0) {
				continue;
			}
replace:
			best = task;
			best_qos = qos;
		}
		if (!cpu_qos)
			return best;
		if (!best) {
			if (!bandwidth || second_pass ||
			    smp_load_acquire(&head->next) == head)
				return NULL;
			second_pass = true;
			continue;
		}
		qos = (u8)get_task_qos(best);
		if (!qos)
			return best;
		index = (unsigned int)qos - 1;
		if (WARN_ON_ONCE(index >= 6))
			return NULL;
		if (bandwidth) {
			runtime = best->se.sum_exec_runtime -
				x80_stock_task_read_u64(best, 0x110);
			if (runtime <= READ_ONCE(rsc_max_qos_runtime_granularity_ns[index]))
				return best;
			if (tracing_is_on() && !READ_ONCE(in_cts_atracetest))
				__rsc_systrace_c(0, NULL, (s64)best->pid,
						 "CPU %d qos_bw_tmo", raw_smp_processor_id());
			WRITE_ONCE(x80_stock_task_oem_mut(best)[0x138], 1);
			if (second_pass || (smp_load_acquire(&head->next) != head &&
					  READ_ONCE(head->next) == READ_ONCE(head->prev)))
				return best;
			continue;
		}
		elapsed = sched_clock() - x80_stock_task_read_u64(best, 0x108);
#ifdef CONFIG_VIVO_X80_UI_BOOST
		if (best == READ_ONCE(rsc_launcher_gc) && READ_ONCE(rsc_in_critical_path))
			return (s64)elapsed > (s64)READ_ONCE(rsc_max_qos_special_enqueue_granularity_ns)
				? NULL : best;
#endif
		return elapsed > READ_ONCE(rsc_max_qos_enqueue_granularity_ns[index])
			? NULL : best;
	}
}

void x80_stock_pick_svp_task(struct rq *rq, struct task_struct **task,
			   struct sched_entity **entity)
{
	struct task_struct *best;
	struct list_head *head = x80_stock_rq_shared_head(rq);
	struct list_head *node;
	bool cpu_qos;
	int qos;
	unsigned int index;
	u64 elapsed;

	if (!*task)
		return;
	cpu_qos = READ_ONCE(rsc_cpu_qos_enable);
	if (x80_stock_task_boost_acquire(*task))
		return;
	if (smp_load_acquire(&head->next) == head)
		return;
	best = x80_stock_pick_first_svp_task(rq);
	if (!best)
		return;
	qos = cpu_qos ? (u8)get_task_qos(best) : 0;
	if (cpu_qos && !qos) {
		x80_stock_qos_clear_pick_flag(best);
		x80_stock_qos_unlink_pick(best);
		x80_stock_qos_pick_subtract_util(rq, best);
		/* Stock has only this decrement; do not invent an enqueue inc. */
		atomic_dec(&rsc_svp_task_cnt);
		WRITE_ONCE(*x80_stock_rq_shared_count(rq),
			   (u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) - 1));
		/* Corrective ordering: trace while list's reference is still ours. */
		x80_stock_trace_svp_queue_op(best, "svp_pick_dequeue");
		put_task_struct(best);
		return;
	}
	/* rq clock domain is intentional; do not replace with sched_clock(). */
	if (rq_clock(rq) < x80_stock_task_read_u64(best, 0xa0))
		return;
	elapsed = rq_clock(rq) - x80_stock_task_read_u64(best, 0xa0);
	if (elapsed < (u64)(s64)READ_ONCE(svp_min_sched_delay_granularity) * 1000000ULL)
		return;
	x80_stock_trace_svp_queue_op(best, "pick_svp");
	*task = best;
	*entity = &best->se;
	if (x80_stock_task_read_u8(best, X80_QOS_FLAGS_D6D) & 0x40) {
		WRITE_ONCE(rsc_svp_preempt_pickup_count,
			   READ_ONCE(rsc_svp_preempt_pickup_count) + 1);
		x80_stock_qos_clear_pick_flag(best);
		if (!cpu_qos) {
			node = x80_stock_task_shared_node(best);
			if (smp_load_acquire(&node->next) != node) {
				x80_stock_qos_unlink_pick(best);
				x80_stock_qos_pick_subtract_util(rq, best);
				WRITE_ONCE(*x80_stock_rq_shared_count(rq),
					(u16)(READ_ONCE(*x80_stock_rq_shared_count(rq)) - 1));
				/* Corrective ordering: list reference protects trace. */
				x80_stock_trace_svp_queue_op(best, "svp_pick_dequeue");
				put_task_struct(best);
			}
		}
	}
	if (!qos)
		return;
	index = (unsigned int)qos - 1;
	if (WARN_ON_ONCE(index >= 6))
		return;
	WRITE_ONCE(*(u64 *)(x80_stock_task_oem_mut(best) + 0x100),
		   sched_clock() + READ_ONCE(rsc_max_qos_granularity_ns[index]));
}

void x80_stock_qos_pick_next(struct rq *rq, struct task_struct **task,
			     struct sched_entity **entity)
{
	struct x80_stock_qos_bw *bw = x80_stock_rq_qos_bw(rq);
	struct task_struct *original = *task;
	unsigned long flags;

	lockdep_assert_held(&rq->lock);
	raw_spin_lock_irqsave(&bw->lock, flags);
	if (x80_stock_qos_bw_throttled(rq))
		goto out;
	x80_stock_pick_svp_task(rq, task, entity);
	/* Stock honors an existing skip buddy of the chosen leaf CFS group. */
	if ((*entity)->cfs_rq->skip == *entity) {
		*task = original;
		*entity = &original->se;
	}
	if (*task != original && !smp_load_acquire(&bw->running))
		qos_bw_period_start(rq);
	WRITE_ONCE(bw->exempt_from_charge, *task == original);
	qos_runnable_stat_record(*task);
out:
	raw_spin_unlock_irqrestore(&bw->lock, flags);
}
