/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_DYNAMIC_SVP_RCU_OWNER_H
#define _X80_STOCK_DYNAMIC_SVP_RCU_OWNER_H
/* Include after genuine sched.h. Recovered producer +195310..1959fc and
 * consumer +47099c..470b30. Caller must hold rcu_read_lock(), as stock
 * inline callers do; lockdep assertion documents, never invents that lock.
 * These are real timer registry references, NOT a second task SVP list. */
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/timer.h>
#include "x80_stock_dynamic_svp_owner.h"
static_assert(offsetof(struct task_struct, rcu_read_lock_nesting) == 0x448);
static_assert(offsetof(struct task_struct, pid) == 0x5c8);
extern int rsc_svp_rcu_blockchain_boost_task_count;
extern atomic_t rsc_svp_rcu_blockchain_boost;
extern struct task_struct *rsc_svp_rcu_blockchain_task[30];
extern struct timer_list rsc_svp_rcu_blockchain_timer;
extern unsigned int rsc_sync_rcu_blockchain_boost_time;
extern unsigned int rsc_sync_rcu_blockchain_kwork_run_unrun_count[2];
extern unsigned int rsc_svp_rcu_blockchain_total_boost_task_count;
extern unsigned int rsc_svp_rcu_blockchain_boost_task_over_count;
extern unsigned int rsc_svp_rcu_blockchain_boost_restore_skip_count;

static inline void x80_stock_rsc_svp_rcu_blockchain_timeout(struct timer_list *t)
{
	int n = READ_ONCE(rsc_svp_rcu_blockchain_boost_task_count), i;

	if (n < 1 || atomic_read(&rsc_svp_rcu_blockchain_boost) < 1)
		return;
	BUG_ON(n > 30);
	for (i = n - 1; i >= 0; --i) {
		struct task_struct *p = READ_ONCE(rsc_svp_rcu_blockchain_task[i]);

		x80_stock___dynamic_svp_dequeue(p, 1);
		put_task_struct(p); /* registry ref, independent of shared-list ref */
	}
	/* Stock leaves stale pointers/count; working gate owns publication. */
	atomic_dec(&rsc_svp_rcu_blockchain_boost);
}

/* Return value is owner DF2 bit7, or 1 when PID lookup fails. Stock takes
 * signed 16-bit PID for >0 predicate, unsigned low16 for find_task_by_vpid.
 * public source prototype deliberately not fabricated. */
static inline int x80_stock_rsc_boost_rcu_blockchain(int raw_pid)
{
	struct task_struct *owner = NULL, *g, *p;
	int result, index;
	bool winner;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(), "SVP RCU owner without read lock");
	if ((s16)raw_pid > 0)
		owner = find_task_by_vpid((u16)raw_pid);
	if (owner) {
		if (owner->sched_class == &fair_sched_class)
			x80_stock_dynamic_svp_enqueue_internal(owner, current, 1, 0,
				"rcu blockchain");
		get_task_struct(owner); /* protects through function return */
	}
	WRITE_ONCE(rsc_sync_rcu_blockchain_boost_time,
		READ_ONCE(rsc_sync_rcu_blockchain_boost_time) + 1);
	winner = atomic_inc_return(&rsc_svp_rcu_blockchain_boost) == 1;
	if (!winner) {
		WRITE_ONCE(rsc_svp_rcu_blockchain_boost_restore_skip_count,
			READ_ONCE(rsc_svp_rcu_blockchain_boost_restore_skip_count) + 1);
		atomic_dec(&rsc_svp_rcu_blockchain_boost);
	} else {
		index = 0;
		if (owner) {
			get_task_struct(owner); /* transferred to timer callback */
			WRITE_ONCE(rsc_svp_rcu_blockchain_task[0], owner);
			index = 1;
		}
		WRITE_ONCE(rsc_svp_rcu_blockchain_boost_task_count, index);
	}
	for_each_process_thread(g, p) {
		if (!READ_ONCE(p->rcu_read_lock_nesting))
			continue;
		if (winner && READ_ONCE(rsc_svp_rcu_blockchain_boost_task_count) > 29) {
			WRITE_ONCE(rsc_svp_rcu_blockchain_boost_task_over_count,
				READ_ONCE(rsc_svp_rcu_blockchain_boost_task_over_count) + 1);
			break;
		}
		if (p->sched_class != &fair_sched_class)
			continue;
		result = x80_stock_dynamic_svp_enqueue_internal(p, current, 1, 0,
			"rcu blockchain");
		if (!winner || result < 0)
			continue;
		WRITE_ONCE(rsc_svp_rcu_blockchain_total_boost_task_count,
			READ_ONCE(rsc_svp_rcu_blockchain_total_boost_task_count) + 1);
		index = READ_ONCE(rsc_svp_rcu_blockchain_boost_task_count);
		BUG_ON(index > 29);
		WRITE_ONCE(rsc_svp_rcu_blockchain_boost_task_count, index + 1);
		WRITE_ONCE(rsc_svp_rcu_blockchain_task[index], p);
		get_task_struct(p); /* paired with callback put */
	}
	if (winner) {
		if (READ_ONCE(rsc_svp_rcu_blockchain_boost_task_count) >= 1) {
			/* Stock initializes only after winning exclusive producer gate. */
			timer_setup(&rsc_svp_rcu_blockchain_timer,
				x80_stock_rsc_svp_rcu_blockchain_timeout, 0);
			mod_timer(&rsc_svp_rcu_blockchain_timer, jiffies + 2);
		} else {
			atomic_dec(&rsc_svp_rcu_blockchain_boost);
		}
	}
	if (!owner)
		return 1;
	result = (smp_load_acquire(x80_stock_task_oem_mut(owner) +
		X80_QOS_SENTINEL_DF2) >> 7) & 1;
	index = result ^ 1;
	WRITE_ONCE(rsc_sync_rcu_blockchain_kwork_run_unrun_count[index],
		READ_ONCE(rsc_sync_rcu_blockchain_kwork_run_unrun_count[index]) + 1);
	put_task_struct(owner);
	return result;
}
#endif
