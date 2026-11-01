/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_BANDWIDTH_H
#define _X80_STOCK_QOS_BANDWIDTH_H
/* Include after the genuine scheduler-private sched.h. No replacement rq/task. */
#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/sched/clock.h>
#include <linux/timer.h>
#include "x80_stock_qos_rq_private.h"

/* The stock 72-byte per-CPU object at 2abb3a0, NOT an rq padding overlay.
 * Stock init_timer_key and __mod_timer prove timer_list, not hrtimer.
 * Names are recovered roles; the C compiler supplies its sole alignment byte.
 */
struct x80_stock_qos_bw {
	raw_spinlock_t lock;
	u8 running;
	u8 throttled;
	u8 exempt_from_charge;
	s32 remaining_ns;
	int cpu;
	struct timer_list period_timer;
};

static_assert(sizeof(raw_spinlock_t) == 4);
static_assert(offsetof(struct x80_stock_qos_bw, lock) == 0);
static_assert(offsetof(struct x80_stock_qos_bw, running) == 4);
static_assert(offsetof(struct x80_stock_qos_bw, throttled) == 5);
static_assert(offsetof(struct x80_stock_qos_bw, exempt_from_charge) == 6);
static_assert(offsetof(struct x80_stock_qos_bw, remaining_ns) == 8);
static_assert(offsetof(struct x80_stock_qos_bw, cpu) == 0xc);
static_assert(offsetof(struct x80_stock_qos_bw, period_timer) == 0x10);
static_assert(sizeof(struct timer_list) == 0x38);
static_assert(sizeof(struct x80_stock_qos_bw) == 0x48);
static_assert(offsetof(struct rq, android_oem_data1[14]) == 0x1238);
static_assert(offsetof(struct task_struct, se.vruntime) == 0x110);
static_assert(offsetof(struct sched_entity, depth) == 0x140);
static_assert(offsetof(struct sched_entity, parent) == 0x148);
static_assert(offsetof(struct sched_entity, cfs_rq) == 0x150);

DECLARE_PER_CPU(struct x80_stock_qos_bw, qos_bw_data);
/* Owned by the enqueue/knob agent, not defined a second time here. */
extern u8 rsc_cpu_qos_enable;
extern u8 rsc_qos_bw_enable;
extern u32 rsc_qos_debug;
extern u64 rsc_max_qos_runtime_granularity_ns[6];
extern u64 rsc_max_qos_enqueue_granularity_ns[6];
extern u32 rsc_max_qos_granularity_ns[6];
extern u32 rsc_qos_bw_quota;
extern u32 rsc_qos_bw_period;

extern u8 qos_heave_runnable_stat;
extern atomic64_t rsc_qos_runnable_count[6][11];
extern atomic64_t rsc_qos_runnable_total_time_count[6];
extern atomic64_t rsc_qos_runnable_total_time[6];

/* No NULL/readiness gate in hot paths: sched_init must publish every owner. */
static inline struct x80_stock_qos_bw *x80_stock_rq_qos_bw(struct rq *rq)
{
	return (struct x80_stock_qos_bw *)(unsigned long)
		READ_ONCE(rq->android_oem_data1[14]);
}

static inline bool x80_stock_qos_bw_throttled(struct rq *rq)
{
	struct x80_stock_qos_bw *bw = x80_stock_rq_qos_bw(rq);

	return READ_ONCE(rsc_qos_bw_enable) &&
		smp_load_acquire(&bw->running) &&
		smp_load_acquire(&bw->throttled);
}

void __rsc_systrace_c(int pid, void *address, s64 value, const char *format, ...);
void qos_bw_data_init(struct rq *rq);
void qos_bw_update_curr(struct rq *rq, u64 delta_exec);
/* Start is caller-serialized by bw->lock, just like both stock fair sites. */
void qos_bw_period_start(struct rq *rq);
void qos_bw_period_stop(struct rq *rq);
/* Corrective CPUHP quiescence, after migrations, outside rq/bw locks. */
void x80_stock_qos_bw_cpu_offline(unsigned int cpu);
void qos_runnable_stat_record(struct task_struct *task);
int rsc_qos_tick_preempt_check(struct rq *rq, struct task_struct *task,
			       unsigned long ideal_runtime);
int rsc_qos_preempt_check(struct task_struct *curr, struct task_struct *wake);
struct task_struct *x80_stock_pick_first_svp_task(struct rq *rq);
void x80_stock_pick_svp_task(struct rq *rq, struct task_struct **task,
			   struct sched_entity **entity);
/* One wrapper for each fair pick branch. rq is locked; it locks bw inside. */
void x80_stock_qos_pick_next(struct rq *rq, struct task_struct **task,
			     struct sched_entity **entity);

#endif
