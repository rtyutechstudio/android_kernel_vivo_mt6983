/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_TICK_H
#define _X80_STOCK_QOS_TICK_H
/* Include after real sched.h. Called AFTER ordinary tick rq unlock and load
 * balancing, before Android scheduler_tick hook. Acquires rq only where stock
 * does so; matching list owners assert caller rq lock, never lock themselves. */
#include <linux/cred.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/x80_stock_qos_private.h>
#include "x80_stock_qos_dequeue_owner.h"

extern u8 rsc_cpu_qos_enable;
extern u8 rsc_qos_bw_enable;
extern u32 rsc_qos_reenqueue_time;
extern u64 rsc_max_qos_enqueue_granularity_ns[6];
extern unsigned int sysctl_rsc_switch;
extern unsigned long rsc_debug;
extern struct cpumask rsc_cpu_littlecore;
extern u32 dynamic_svp_timeout_count[7];
extern u32 dynamic_svp_tick_hit, dynamic_svp_tick_rt_thread_hit;
extern u32 dynamic_svp_timeout_total_count;
extern u32 rsc_preempt_disable_overtime_count;
extern u32 rsc_preempt_disable_overjifftime_count;
extern u32 rsc_preempt_disable_overruntime_count;
/* Genuine external policy, not a replacement stub: parent must close owner. */
void trigger_svp_balance(struct rq *rq, int cpu_qos_enabled);
void __rsc_systrace_c(int pid, void *address, s64 value,
		const char *format, ...);
extern int rsc_top_task_group_id;
extern u32 rsc_preempt_disable_writelock_max_time_jiffies;
extern u32 css_id_to_group_id[32];

/* Exact stock diagnostic branch1d2f30..31ec; repeated jiffies/CSS/preempt
 * observations remain independent (do not cache into one snapshot). */
static inline void x80_stock_tick_preempt_timeout_diagnostic(
		struct task_struct *task, u64 jiffies_delta, u64 runtime_delta)
{
	struct cgroup_subsys_state *css;
	unsigned long expires = x80_stock_task_read_u64(task, X80_QOS_SVP_EXPIRES);
	u32 max_time = READ_ONCE(rsc_preempt_disable_writelock_max_time_jiffies);
	unsigned long first_jiffies = jiffies;
	unsigned long second_jiffies = jiffies;
	unsigned int pc1 = preempt_count();
	unsigned int pc2 = preempt_count();
	bool interrupts_enabled = !irqs_disabled();
	bool need_resched = READ_ONCE(task->thread_info.flags) & _TIF_NEED_RESCHED;
	unsigned int uid = __kuid_val(task->cred->uid);
	int first_id, second_id, top;
	u32 group_id;
	u64 remaining;

	css = task_css(task, cpu_cgrp_id);
	first_id = css ? READ_ONCE(css->id) : 1;
	top = first_id == READ_ONCE(rsc_top_task_group_id);
	css = task_css(task, cpu_cgrp_id);
	second_id = css ? READ_ONCE(css->id) : 1;
	/* Raw maps id>31 to2; NULL maps1 without the range branch. */
	group_id = (unsigned int)second_id > 31 ? 2 :
		READ_ONCE(css_id_to_group_id[second_id]);
	remaining = x80_stock_task_read_u64(task, X80_QOS_SVP_RUNTIME_END) -
		READ_ONCE(task->se.sum_exec_runtime);
	printk("RSC forcereched update_process_times toolong currenttime: %6lu ms preempt_count: %6x %d count: %d resched%d %d uid %5d %16s %5d top%d g%d pol %d prio %3d %3d %3d (%16s %5d) over_count: %u pdtm%d pdrtm%d elp %2d exe left %2ld ms\n",
		second_jiffies << 2, pc1, pc2 & 0xff, interrupts_enabled,
		!!need_resched, READ_ONCE(x80_stock_task_oem(task)[X80_QOS_SVP_ACTIVE]),
		uid, task->comm, task->pid, top, group_id,
		READ_ONCE(task->policy), READ_ONCE(task->prio),
		READ_ONCE(task->static_prio), READ_ONCE(task->normal_prio),
		task->group_leader->comm, task->group_leader->pid,
		READ_ONCE(rsc_preempt_disable_overtime_count),
		!(jiffies_delta >> 63), !(runtime_delta >> 63),
		max_time + ((u32)first_jiffies - (u32)expires) * 4,
		(long)(remaining / 1000000ULL));
	dump_stack();
}

static inline void x80_stock_tick_increment(u32 *count)
{
	WRITE_ONCE(*count, READ_ONCE(*count) + 1);
}
static inline void x80_stock_tick_clear_qos_floor(struct task_struct *task)
{
	u8 *oem = x80_stock_task_oem_mut(task);
	WRITE_ONCE(oem[X80_QOS_FLOOR_E46], 0);
	WRITE_ONCE(*(u32 *)(oem + 0x130), 0); /* e40, leaves e44 intact */
}
static inline bool x80_stock_tick_node_empty(struct task_struct *task)
{
	struct list_head *node = x80_stock_task_shared_node(task);
	return smp_load_acquire(&node->next) == node;
}

/* Tick expiry is NOT dequeue expiry: all source timestamps are reached and
 * every active nibble is counted, with no local-boost clearing or early exit. */
static inline u64 x80_stock_tick_clear_dynamic(struct task_struct *task)
{
	u64 boost = x80_stock_task_boost_acquire(task);
	u8 *oem = x80_stock_task_oem_mut(task);
	unsigned int i;
	x80_stock_tick_increment(&dynamic_svp_timeout_total_count);
	WRITE_ONCE(*(u16 *)(oem + 0xe0), 0);
	for (i = 0; i < 7; i++) {
		if (i >= 1 && i <= 4)
			x80_stock_enqueue_store_u64(task, 0xb8 + 8 * i, 0);
		if (boost & (0xfULL << (i * 4)))
			x80_stock_tick_increment(&dynamic_svp_timeout_count[i]);
	}
	WRITE_ONCE(oem[X80_QOS_SENTINEL_DF2], 0xff);
	x80_stock_enqueue_store_u64(task, X80_QOS_BOOST_DB8, 0);
	return boost;
}

static inline void x80_stock_tick_expire_dynamic(struct rq *rq,
		struct task_struct *task, u64 now, u8 cpu_qos, bool forced)
{
	struct rq_flags rf;
	u8 depth;
	u64 boost;
	int qos;
	u8 *oem = x80_stock_task_oem_mut(task);

	rq_lock(rq, &rf);
	/* Raw CMN now,~timestamp; B.MI tests bit63(now-timestamp-1).
	 * No signed operand relation or unsigned deadline comparison. */
	if (!forced && ((now + ~x80_stock_task_read_u64(task, 0xb0)) >> 63)) {
		rq_unlock(rq, &rf);
		return;
	}
	if (!forced)
		x80_stock_tick_increment(&dynamic_svp_tick_hit);
	boost = x80_stock_tick_clear_dynamic(task);
	depth = READ_ONCE(oem[0x5e]);
	WRITE_ONCE(oem[0x5e], 0);
	qos = get_task_qos(task);
	if (!x80_stock_tick_node_empty(task) &&
	    (!cpu_qos || (boost & 6) || !qos ||
	     !READ_ONCE(oem[X80_QOS_LAST_ACCOUNTED])))
		x80_stock_dequeue_svp_task(rq, task);
	rq_unlock(rq, &rf);
	x80_stock_tick_clear_qos_floor(task);
	printk("svp tick uid %5d %16s %5d (%16s %5d) dynamic_svp %08llx rsc_svp %x boost %x qos%d depth %u hit %u timeout %u\n",
		__kuid_val(task->cred->uid), task->comm, task->pid,
		task->group_leader->comm, task->group_leader->pid, boost,
		READ_ONCE(oem[X80_QOS_FLAGS_D6D]),
		READ_ONCE(oem[X80_QOS_FLAGS_D6F]), qos, depth,
		READ_ONCE(dynamic_svp_tick_hit),
		READ_ONCE(dynamic_svp_timeout_total_count));
}

static inline void x80_stock_tick_accounted_policy(struct rq *rq,
		struct task_struct *task, u64 now)
{
	u8 *oem = x80_stock_task_oem_mut(task);
	unsigned int qos = READ_ONCE(oem[X80_QOS_LAST_ACCOUNTED]);
	bool expired = false;
	struct rq_flags rf;
	u64 start, interval;
	int raw_qos, multiplier;

	if (!qos) {
		if (READ_ONCE(task->se.sum_exec_runtime) -
		    READ_ONCE(task->se.prev_sum_exec_runtime) < 1000000001ULL)
			goto maybe_reenqueue;
	} else {
		if (x80_stock_tick_node_empty(task))
			goto maybe_reenqueue;
		if ((x80_stock_task_read_u64(task, 0x100) - now) >> 63)
			expired = true;
		else {
			/* Corrective validation instead of raw BUG for arbitrary e44. */
			if (WARN_ON_ONCE(qos - 1 >= 6))
				return;
			start = x80_stock_task_read_u64(task, 0x108);
			interval = READ_ONCE(rsc_max_qos_enqueue_granularity_ns[qos - 1]);
			expired = (start - now + interval) >> 63;
		}
		if ((u8)READ_ONCE(rsc_qos_bw_enable))
			expired = (x80_stock_task_read_u64(task, 0x108) - now +
				   10000000000ULL) >> 63;
		if (!x80_stock_tick_node_empty(task) && expired) {
			if (READ_ONCE(task->sched_class) != &fair_sched_class)
				return;
			if ((u8)READ_ONCE(rsc_qos_bw_enable))
				__rsc_systrace_c(0, NULL, task->pid,
						"CPU %d qos_bw_tmo_deq", smp_processor_id());
			rq_lock(rq, &rf);
			x80_stock_dequeue_svp_task(rq, task);
			rq_unlock(rq, &rf);
			x80_stock_tick_clear_qos_floor(task);
			return;
		}
	}
maybe_reenqueue:
	if ((u8)READ_ONCE(rsc_qos_bw_enable))
		return;
	multiplier = READ_ONCE(rsc_qos_reenqueue_time);
	if (!multiplier || qos || !x80_stock_tick_node_empty(task))
		return;
	raw_qos = get_task_qos(task);
	if (raw_qos < 3)
		return;
	if (WARN_ON_ONCE((unsigned int)(raw_qos - 1) >= 6))
		return;
	start = x80_stock_task_read_u64(task, 0x108);
	interval = READ_ONCE(rsc_max_qos_enqueue_granularity_ns[raw_qos - 1]);
	/* Raw uses zero-extended w multiplier and unsigned 64-bit MADD. */
	if (!((start - now + interval * (u32)multiplier) >> 63))
		return;
	if (READ_ONCE(task->sched_class) != &fair_sched_class)
		return;
	if (READ_ONCE(rsc_debug) & (1U << 28))
		printk_deferred("RSC QOS tick reequeue uid %5d %16s %5d (%16s %5d) qos%d overtime: %llu gran: %llu\n",
			__kuid_val(task->cred->uid), task->comm, task->pid,
			task->group_leader->comm, task->group_leader->pid,
			raw_qos, (now - start) / 1000000ULL,
			interval / 1000000ULL);
	rq_lock(rq, &rf);
	if (READ_ONCE(task->sched_class) == &fair_sched_class) {
		x80_stock_enqueue_store_u64(task, 0x108, now);
		x80_stock_enqueue_svp_task(rq, task);
	}
	rq_unlock(rq, &rf);
}

static inline void x80_stock_tick_legacy(struct task_struct *task, u64 now)
{
	u8 *oem = x80_stock_task_oem_mut(task);
	u64 jd, rd;
	unsigned long tick;

	if (READ_ONCE(oem[X80_QOS_FLOOR_E46]) &&
	    ((x80_stock_task_read_u64(task, 0x128) - now) >> 63))
		x80_stock_tick_clear_qos_floor(task);
	/* Original unrelated UI-promotion timer branch lives in the separately
	 * unbuilt x80_stock_tick_ui_optional.h recovery candidate. The actual
	 * subsystem/interface is absent in #15; no fake enable=0 is defined. */
	if (!READ_ONCE(oem[X80_QOS_SVP_ACTIVE]) ||
	    !(READ_ONCE(task->thread_info.flags) & _TIF_NEED_RESCHED) ||
	    !READ_ONCE(oem[X80_QOS_SVP_SENTINEL]))
		return;
	tick = jiffies;
	jd = tick - x80_stock_task_read_u64(task, X80_QOS_SVP_EXPIRES);
	rd = READ_ONCE(task->se.sum_exec_runtime) -
	     x80_stock_task_read_u64(task, X80_QOS_SVP_RUNTIME_END);
	/* Raw TST rd,jd; B.LT after TST(V=0): both deltas negative -> skip. */
	if ((jd & rd) >> 63)
		return;
	x80_stock_tick_increment(&rsc_preempt_disable_overtime_count);
	if (!(jd >> 63))
		x80_stock_tick_increment(&rsc_preempt_disable_overjifftime_count);
	if (!(rd >> 63))
		x80_stock_tick_increment(&rsc_preempt_disable_overruntime_count);
	if (READ_ONCE(rsc_debug) & (1U << 14))
		x80_stock_tick_preempt_timeout_diagnostic(task, jd, rd);
	WRITE_ONCE(oem[X80_QOS_SVP_SENTINEL], 0);
	x80_stock_enqueue_store_u64(task, X80_QOS_SVP_EXPIRES, jiffies);
	set_bit(30, &task->thread_info.flags);
}

static inline void x80_stock_qos_scheduler_tick(struct rq *rq, int cpu,
		u64 entry_now, u8 entry_cpu_qos)
{
	struct task_struct *task = current;
	u64 boost, total, high, capacity;
	unsigned int policy;

	/* rq aggregate reads occur unlocked, as stock. Do not cache two groups
	 * together: second group is re-read only after the first exceeds limit. */
	if (READ_ONCE(*x80_stock_rq_shared_count(rq)) >= 2) {
		if (!entry_cpu_qos)
			trigger_svp_balance(rq, 0);
		else {
			total = READ_ONCE(rq->android_oem_data1[9]) +
				READ_ONCE(rq->android_oem_data1[10]) +
				READ_ONCE(rq->android_oem_data1[11]) +
				READ_ONCE(rq->android_oem_data1[12]) +
				READ_ONCE(rq->android_oem_data1[13]);
			capacity = READ_ONCE(rq->cpu_capacity_orig) << 10;
			if (capacity < total * 1078ULL) {
				high = READ_ONCE(rq->android_oem_data1[10]) +
					READ_ONCE(rq->android_oem_data1[11]) +
					READ_ONCE(rq->android_oem_data1[12]) +
					READ_ONCE(rq->android_oem_data1[13]);
				if (capacity >= high * 1078ULL) {
					trigger_svp_balance(rq, entry_cpu_qos);
					goto task_policy;
				}
			}
			goto cpu_high;
		}
	} else {
cpu_high:
		if (entry_cpu_qos && ((READ_ONCE(rsc_cpu_littlecore.bits[0]) >> cpu) & 1)) {
			high = READ_ONCE(rq->android_oem_data1[12]) +
				READ_ONCE(rq->android_oem_data1[13]);
			if (high * 1078ULL > (READ_ONCE(rq->cpu_capacity_orig) << 10))
				trigger_svp_balance(rq, entry_cpu_qos);
		}
	}
task_policy:
	boost = x80_stock_task_boost_acquire(task);
	if (boost) {
		boost = x80_stock_task_boost_acquire(task); /* required second read */
		if (boost & 0xf0000000ULL) {
			if (!(READ_ONCE(sysctl_rsc_switch) & (1U << 14)))
				x80_stock_tick_expire_dynamic(rq, task, entry_now,
						      entry_cpu_qos, true);
		} else if (READ_ONCE(rq->cfs.h_nr_running) >= 2 &&
			   ((x80_stock_task_read_u64(task, 0xb0) - entry_now) >> 63)) {
			policy = READ_ONCE(task->policy);
			if ((unsigned int)(policy - 1) <= 1) {
				x80_stock_tick_increment(&dynamic_svp_tick_rt_thread_hit);
				if (!(READ_ONCE(rsc_debug) & (1U << 21)))
					goto legacy;
			}
			x80_stock_tick_expire_dynamic(rq, task, entry_now,
						      entry_cpu_qos, false);
		}
	} else if (READ_ONCE(rq->cfs.h_nr_running) >= 2) {
		x80_stock_tick_accounted_policy(rq, task, entry_now);
	}
legacy:
	x80_stock_tick_legacy(task, entry_now);
}
#endif
