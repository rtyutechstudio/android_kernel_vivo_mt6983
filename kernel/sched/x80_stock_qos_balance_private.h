/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_BALANCE_PRIVATE_H
#define _X80_STOCK_QOS_BALANCE_PRIVATE_H
/* Include after genuine scheduler sched.h. Recovered stock storage owners. */
#include <linux/stop_machine.h>
#include <linux/sched/task.h>
#include "x80_stock_qos_enqueue_owner.h"
static_assert(sizeof(struct cpu_stop_work) == 40);
static_assert(offsetof(struct rq, android_oem_data1[3]) == 0x11e0);
static_assert(offsetof(struct rq, android_oem_data1[8]) == 0x1208);
static_assert(offsetof(struct rq, clock_task) == 0xbc0);
static_assert(offsetof(struct task_struct, se.exec_start) == 0x100);
static_assert(offsetof(struct task_struct, on_cpu) == 0x4c);
static_assert(offsetof(struct task_struct, policy) == 0x430);

/* Stock Image+2e1bb04 BSS. Genuine legacy diagnostic counter: only proved
 * writer is pick_svp_task atomic_dec (1ee484/1ee6e0). No increment or bit6
 * admission setter recovered. NOT a balanced total, NOT rq11ca alias.
 * Define once in actual balance globals; do not "correct" its arithmetic. */
extern atomic_t rsc_svp_task_cnt;
extern int svp_min_migration_delay;
extern int rsc_first_candidate_pid;
extern int rsc_second_candidate_pid;
extern unsigned int rsc_heavy_task_push_cont_count;
extern unsigned int rsc_heavy_task_push_skip_count;
extern unsigned int rsc_heavy_task_push_suc_count;
DECLARE_PER_CPU(struct task_struct *, svp_balance_task);

static inline u8 *x80_stock_svp_balance_busy(struct rq *rq)
{
	return (u8 *)rq->android_oem_data1;
}
static inline struct cpu_stop_work *x80_stock_svp_balance_work(struct rq *rq)
{
	return (struct cpu_stop_work *)&rq->android_oem_data1[3];
}
static inline bool x80_stock_svp_task_cache_hot(struct rq *rq,
		struct task_struct *p)
{
	int cost;

	if (p->sched_class != &fair_sched_class || p->policy == SCHED_IDLE)
		return false;
	cost = READ_ONCE(sysctl_sched_migration_cost);
	if (cost == -1)
		return true;
	return cost && (s64)(rq->clock_task - p->se.exec_start) < cost;
}

/* Stock +471344..471454 (QoS) and +471860..4718b4 (SVP-only).
 * Qualifier here is D6D bit0 and wait-age, NOT legacy bit6.
 * The QoS two-best ordering is descending level then descending wait age.
 * It normally picks second-best when two exist, preserving strongest local
 * candidate; stock special case level6+following task picks best immediately. */
static inline struct task_struct *x80_stock_svp_balance_candidate_locked(
		struct rq *rq, u8 cpu_qos_sample)
{
	struct list_head *head = x80_stock_rq_shared_head(rq), *node;
	struct task_struct *best = NULL, *second = NULL, *p;
	u64 best_age = 0, second_age = 0, age, min_age;
	u8 best_qos = 0, second_qos = 0, qos;

	lockdep_assert_held(&rq->lock);
	if (!cpu_qos_sample) {
		min_age = (u64)(s64)READ_ONCE(svp_min_migration_delay) * 1000000;
		list_for_each(node, head) {
			p = (struct task_struct *)((u8 *)node - 0xda0);
			if (!(x80_stock_task_read_u8(p, X80_QOS_FLAGS_D6D) & 1))
				continue;
			age = rq->clock - x80_stock_task_read_u64(p, X80_SVP_RQ_CLOCK);
			if (age >= min_age)
				return p;
		}
		return NULL;
	}
	list_for_each(node, head) {
		p = (struct task_struct *)((u8 *)node - 0xda0);
		if (p == current)
			continue;
		qos = (u8)get_task_qos(p);
		/* Check is BEFORE testing current candidate QoS; stock early path. */
		if (best_qos == 6 && best)
			return best;
		if (qos < 2)
			continue;
		age = rq->clock - x80_stock_task_read_u64(p, X80_QOS_ENQUEUE_CLOCK);
		if (!best || qos > best_qos || (qos == best_qos && age > best_age)) {
			second = best;
			second_qos = best_qos;
			second_age = best_age;
			best = p;
			best_qos = qos;
			best_age = age;
		} else if (qos > second_qos || (qos == second_qos && age > second_age)) {
			second = p;
			second_qos = qos;
			second_age = age;
		}
	}
	return second ? second : best;
}
#endif
