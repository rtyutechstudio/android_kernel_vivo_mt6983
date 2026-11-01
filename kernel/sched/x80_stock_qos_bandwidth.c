// SPDX-License-Identifier: GPL-2.0
/* Finite stock QoS bandwidth/runnable/preemption owner candidate.
 * This TU must be built-in. It does not modify any target .ko or force CRCs.
 */
#include "sched.h"
#include "x80_stock_qos_bandwidth.h"
#include <linux/cgroup.h>
#include <linux/cpuhotplug.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>

DEFINE_PER_CPU(struct x80_stock_qos_bw, qos_bw_data);
u8 qos_heave_runnable_stat;
atomic64_t rsc_qos_runnable_count[6][11];
atomic64_t rsc_qos_runnable_total_time_count[6];
atomic64_t rsc_qos_runnable_total_time[6];

extern int rsc_top_task_group_id;
/* Real stock CTS control state. No definition or fake zero supplied here. */
extern int in_cts_atracetest;

static void x80_stock_qos_bw_trace(const char *format, int cpu, s64 value)
{
	if (tracing_is_on() && !READ_ONCE(in_cts_atracetest))
		__rsc_systrace_c(0, NULL, value, format, cpu);
}

static void qos_bw_period_end(struct timer_list *timer)
{
	struct x80_stock_qos_bw *bw = from_timer(bw, timer, period_timer);
	unsigned long flags;

	raw_spin_lock_irqsave(&bw->lock, flags);
	WRITE_ONCE(bw->running, 0);
	WRITE_ONCE(bw->throttled, 0);
	x80_stock_qos_bw_trace("CPU%d qos_bw_throttled", bw->cpu, 0);
	x80_stock_qos_bw_trace("CPU%d qos_bw_period", bw->cpu, 0);
	raw_spin_unlock_irqrestore(&bw->lock, flags);
}

void qos_bw_data_init(struct rq *rq)
{
	struct x80_stock_qos_bw *bw;

	if (!rq)
		return;
	/* Called once per POSSIBLE CPU in sched_init, after rq->cpu is set.
	 * Static per-CPU allocation is the stock lifetime, not kmalloc or an
	 * inline rq overlay. Never call again on an active/initialized timer.
	 */
	bw = &per_cpu(qos_bw_data, cpu_of(rq));
	raw_spin_lock_init(&bw->lock);
	WRITE_ONCE(bw->running, 0);
	WRITE_ONCE(bw->throttled, 0);
	bw->cpu = cpu_of(rq);
	timer_setup(&bw->period_timer, qos_bw_period_end, 0);
	/* Objects begin zeroed by genuine static perCPU storage: remaining and
	 * exemption need no invented init or task/rq padding writes.
	 */
	smp_store_release(&rq->android_oem_data1[14], (unsigned long)bw);
}

void qos_bw_period_start(struct rq *rq)
{
	struct x80_stock_qos_bw *bw = x80_stock_rq_qos_bw(rq);
	u32 quota;

	lockdep_assert_held(&bw->lock);
	WRITE_ONCE(bw->running, 1);
	WRITE_ONCE(bw->throttled, 0);
	/* MUL W gives exact 32-bit wrap, not a saturation or a u64 quota. */
	quota = (u32)READ_ONCE(rsc_qos_bw_quota) * 1000000U;
	WRITE_ONCE(bw->remaining_ns, (s32)quota);
	mod_timer(&bw->period_timer,
		  jiffies + msecs_to_jiffies(READ_ONCE(rsc_qos_bw_period)));
	x80_stock_qos_bw_trace("CPU%d qos_bw_period", bw->cpu, 1);
}

void qos_bw_update_curr(struct rq *rq, u64 delta_exec)
{
	struct x80_stock_qos_bw *bw = x80_stock_rq_qos_bw(rq);
	unsigned long flags;
	s32 remaining;

	raw_spin_lock_irqsave(&bw->lock, flags);
	if (smp_load_acquire(&bw->running) &&
	    !smp_load_acquire(&bw->exempt_from_charge)) {
		remaining = (s32)((u32)smp_load_acquire(&bw->remaining_ns) -
				  (u32)delta_exec);
		if (remaining <= 0 && !smp_load_acquire(&bw->throttled)) {
			WRITE_ONCE(bw->throttled, 1);
			x80_stock_qos_bw_trace("CPU%d qos_bw_throttled", bw->cpu, 1);
		}
		WRITE_ONCE(bw->remaining_ns, remaining);
	}
	raw_spin_unlock_irqrestore(&bw->lock, flags);
}

void qos_bw_period_stop(struct rq *rq)
{
	struct x80_stock_qos_bw *bw = x80_stock_rq_qos_bw(rq);
	unsigned long flags;

	raw_spin_lock_irqsave(&bw->lock, flags);
	if (smp_load_acquire(&bw->running)) {
		/* Stock runtime stop is nonblocking while holding owner lock.
		 * del_timer_sync here would deadlock the callback on that lock.
		 */
		del_timer(&bw->period_timer);
		WRITE_ONCE(bw->running, 0);
		x80_stock_qos_bw_trace("CPU%d qos_bw_throttled", bw->cpu, 0);
		x80_stock_qos_bw_trace("CPU%d qos_bw_period", bw->cpu, 0);
	}
	raw_spin_unlock_irqrestore(&bw->lock, flags);
}

void x80_stock_qos_bw_cpu_offline(unsigned int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct x80_stock_qos_bw *bw = x80_stock_rq_qos_bw(rq);
	unsigned long flags;

	/* Corrective lifecycle addition: caller has migrated/quiesced CPU and
	 * stopped scheduler producers. Hold neither rq nor owner lock here.
	 * Callback takes ONLY owner lock, never rq; synchronously drain it.
	 */
	/* This Linux 5.10 tree has no lockdep_assert_not_held() helper. */
	qos_bw_period_stop(rq);
	del_timer_sync(&bw->period_timer);
	raw_spin_lock_irqsave(&bw->lock, flags);
	WRITE_ONCE(bw->throttled, 0);
	WRITE_ONCE(bw->exempt_from_charge, 0);
	raw_spin_unlock_irqrestore(&bw->lock, flags);
	/* Retain the valid static perCPU object and published pointer for
	 * online reuse: no free/reallocate/UAF and no timer reinitialization.
	 */
}

/* CPUHP BP prepare's teardown runs on a live control CPU AFTER target has
 * reached DEAD, including task migration and timer migration. That is the
 * sleepable lock-free context required by a flags=0 timer_list sync drain.
 * Do not invoke this from sched_cpu_dying (stop_machine IRQ-off context).
 */
static int x80_stock_qos_bw_dead(unsigned int cpu)
{
	x80_stock_qos_bw_cpu_offline(cpu);
	return 0;
}

static int __init x80_stock_qos_bw_cpuhp_init(void)
{
	int state = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN,
					     "sched/x80-qos:prepare", NULL,
					     x80_stock_qos_bw_dead);

	return state < 0 ? state : 0;
}
early_initcall(x80_stock_qos_bw_cpuhp_init);

static int x80_stock_qos_record_level(struct task_struct *task)
{
	struct cgroup_subsys_state *css;
	int id;

	if (READ_ONCE(rsc_cpu_qos_enable))
		return x80_stock_task_read_u8(task, X80_QOS_LAST_ACCOUNTED);
	if (x80_stock_task_read_u8(task, X80_QOS_FLAGS_D6D) & 3) {
		rcu_read_lock();
		css = task_css(task, cpu_cgrp_id);
		id = css ? css->id : 1;
		rcu_read_unlock();
		if (id == READ_ONCE(rsc_top_task_group_id) ||
		    (x80_stock_task_read_u8(task, X80_QOS_FLAGS_D6F) & 5))
			return 5;
	}
	return get_task_qos(task);
}

void qos_runnable_stat_record(struct task_struct *task)
{
	u64 now = sched_clock();
	u64 enqueued = x80_stock_task_read_u64(task, 0x108); /* absolute e18 */
	u64 last = x80_stock_task_read_u64(task, 0x118); /* absolute e28 */
	u64 elapsed = now - max(enqueued, last);
	int qos = x80_stock_qos_record_level(task);
	unsigned int index, bin;

	if (!qos)
		return;
	index = (unsigned int)qos - 1;
	/* Explicit safe divergence for malformed QoS >=7: stock some guards
	 * admit index6 although physical arrays have six rows. Never scribble.
	 */
	if (WARN_ON_ONCE(index >= 6))
		return;
	if (READ_ONCE(qos_heave_runnable_stat) == 1) {
		/* Stock shifts u64 then truncates W, including wrap at 2^52ns. */
		bin = (u32)(elapsed >> 20);
		if (bin >= 10)
			bin = 10;
		atomic64_inc(&rsc_qos_runnable_count[index][bin]);
	}
	if (elapsed) {
		atomic64_inc(&rsc_qos_runnable_total_time_count[index]);
		atomic64_add(elapsed, &rsc_qos_runnable_total_time[index]);
	}
}

/* Match task sched_entity ancestors exactly as stock fair hierarchy loops. */
static void x80_stock_qos_match_entities(struct sched_entity **a,
				       struct sched_entity **b)
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

static bool x80_stock_qos_expired(struct task_struct *task, int qos, u64 now,
				bool bandwidth)
{
	u64 runtime = task->se.sum_exec_runtime -
		x80_stock_task_read_u64(task, 0x110); /* absolute e20 */
	unsigned int index = (unsigned int)qos - 1;

	if (WARN_ON_ONCE(index >= 6))
		return true;
	if (runtime > READ_ONCE(rsc_max_qos_runtime_granularity_ns[index]))
		WRITE_ONCE(x80_stock_task_oem_mut(task)[0x138], 1); /* e48 */
	if (bandwidth)
		return x80_stock_task_read_u8(task, 0x138) != 0;
	if (!x80_stock_delta_nonnegative(x80_stock_task_read_u64(task, 0x100), now))
		return true; /* absolute e10 deadline */
	return (s64)(x80_stock_task_read_u64(task, 0x108) - now +
		READ_ONCE(rsc_max_qos_enqueue_granularity_ns[index])) < 0;
}

int rsc_qos_tick_preempt_check(struct rq *rq, struct task_struct *task,
			       unsigned long ideal_runtime)
{
	struct list_head *head = x80_stock_rq_shared_head(rq);
	struct list_head *node = x80_stock_task_shared_node(task);
	struct list_head *first, *cursor;
	struct sched_entity *curr_se, *other_se;
	struct task_struct *other;
	int qos = get_task_qos(task), other_qos;
	bool task_linked = smp_load_acquire(&node->next) != node;
	bool have_list;
	u8 bandwidth;
	u64 now;

	first = smp_load_acquire(&head->next);
	now = sched_clock();
	have_list = first != head;
	bandwidth = READ_ONCE(rsc_qos_bw_enable);
	if (x80_stock_qos_bw_throttled(rq))
		return -1;
	if (!qos || !task_linked)
		return have_list ? 1 : -1;
	if (x80_stock_qos_expired(task, qos, now, bandwidth)) {
		if (!have_list)
			return -1;
		/* Stock treats an empty head as true in its second snapshot, or
		 * compares next!=prev to distinguish singleton from competitors.
		 */
		if (smp_load_acquire(&head->next) == head)
			return 1;
		return READ_ONCE(head->next) != READ_ONCE(head->prev) ? 1 : -1;
	}
	if (smp_load_acquire(&head->next) == head)
		return -1;
	list_for_each(cursor, head) {
		other = (struct task_struct *)((u8 *)cursor -
					       X80_TASK_OEM_BASE - X80_QOS_SHARED_NODE);
		other_qos = get_task_qos(other);
		if (bandwidth && x80_stock_task_read_u8(other, 0x138))
			continue;
		if (other_qos > qos)
			return 1;
		if (other_qos != qos)
			continue;
		curr_se = &task->se;
		other_se = &other->se;
		x80_stock_qos_match_entities(&curr_se, &other_se);
		if (curr_se->vruntime - other_se->vruntime > ideal_runtime)
			return 1; /* Stock CMP unsigned B.HI, not signed delta. */
	}
	return 0;
}

int rsc_qos_preempt_check(struct task_struct *curr, struct task_struct *wake)
{
	struct list_head *node = x80_stock_task_shared_node(curr);
	struct list_head *wake_node = x80_stock_task_shared_node(wake);
	bool curr_qos = smp_load_acquire(&node->next) != node && get_task_qos(curr);
	bool wake_linked = smp_load_acquire(&wake_node->next) != wake_node;
	u8 bandwidth = READ_ONCE(rsc_qos_bw_enable);
	int curr_level, wake_level;
	unsigned int index;

	if (bandwidth && x80_stock_qos_bw_throttled(rq_of(curr->se.cfs_rq)))
		return -1;
	if (!curr_qos)
		return wake_linked ? 1 : -1;
	if (!wake_linked)
		return 0;
	curr_level = get_task_qos(curr);
	wake_level = get_task_qos(wake);
	if (bandwidth && curr_level) {
		index = (unsigned int)curr_level - 1;
		if (WARN_ON_ONCE(index >= 6))
			return 1;
		if (curr->se.sum_exec_runtime - x80_stock_task_read_u64(curr, 0x110) >
		    READ_ONCE(rsc_max_qos_runtime_granularity_ns[index])) {
			WRITE_ONCE(x80_stock_task_oem_mut(curr)[0x138], 1);
			return 1;
		}
	}
	if (wake_level > curr_level)
		return 1;
	return wake_level == curr_level ? -1 : 0;
}
