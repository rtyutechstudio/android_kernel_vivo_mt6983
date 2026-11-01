/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_RUNNING_OWNER_H
#define _X80_STOCK_QOS_RUNNING_OWNER_H
#include <linux/cgroup.h>
#include <linux/sched/clock.h>
#include <linux/x80_stock_qos_private.h>

extern u8 rsc_cpu_qos_enable;
extern int rsc_top_task_group_id;
extern atomic64_t rsc_qos_enqueue_count[6];
extern atomic64_t rsc_qos_enqueue_total_time[6];

static_assert(offsetof(struct task_struct, cgroups) == 0x990);
static_assert(offsetof(struct css_set, subsys[cpu_cgrp_id]) == 8);
static_assert(offsetof(struct cgroup_subsys_state, id) == 0x50);

static inline int x80_stock_qos_record_level(struct task_struct *task)
{
	const u8 *oem = x80_stock_task_oem(task);
	struct cgroup_subsys_state *css;
	int id;

	if (READ_ONCE(rsc_cpu_qos_enable))
		return READ_ONCE(oem[X80_QOS_LAST_ACCOUNTED]);
	if (READ_ONCE(oem[X80_QOS_FLAGS_D6D]) & 3) {
		rcu_read_lock();
		css = task_css(task, cpu_cgrp_id);
		id = css ? css->id : 1;
		rcu_read_unlock();
		if (id == READ_ONCE(rsc_top_task_group_id) ||
		    (READ_ONCE(oem[X80_QOS_FLAGS_D6F]) & 5))
			return 5;
	}
	return get_task_qos(task);
}

static inline void x80_stock_qos_running_stat_record_at(
		struct task_struct *task, u64 now)
{
	u64 elapsed = now - x80_stock_task_read_u64(task, 0x108);
	int qos = x80_stock_qos_record_level(task);
	unsigned int index;

	/* CMP elapsed,#1; B.LT is a SIGNED comparison of modular elapsed.
	 * Unlike expiry this checks N!=V, not merely the subtraction N bit. */
	if ((s64)elapsed < 1 || !qos)
		return;
	index = (unsigned int)qos - 1;
	/* Deliberate corrective difference: raw stock BUG guard is >=7 even
	 * though both arrays contain SIX elements. Reject index6 as well; no
	 * caller invariant currently proves arbitrary floor bytes <=6. This
	 * branch is NOT claimed equivalent for stock malformed QoS>=7. */
	if (WARN_ON_ONCE(index >= 6))
		return;
	atomic64_inc(&rsc_qos_enqueue_count[index]);
	atomic64_add(elapsed, &rsc_qos_enqueue_total_time[index]);
}

static inline void x80_stock_qos_running_stat_record(struct task_struct *task)
{
	u64 now = sched_clock();
	x80_stock_qos_running_stat_record_at(task, now);
}
/* Source integration may name the wrapper qos_running_stat_record to retain
 * original exported prototype if required by existing consumer declarations. */
#endif
