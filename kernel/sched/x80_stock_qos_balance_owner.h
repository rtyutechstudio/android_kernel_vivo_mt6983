/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_BALANCE_OWNER_H
#define _X80_STOCK_QOS_BALANCE_OWNER_H
#include "x80_stock_qos_balance_private.h"

/* Reconstructed stock target-selection workers, used by callback owner.
 * Source private helpers must be included with genuine core scheduler
 * definitions; no replacement dequeue/enqueue or standalone exported stubs. */
static inline unsigned long x80_stock_qos_balance_load(struct rq *rq, u8 qos)
{
	unsigned long sum = 0;
	unsigned int index;

	if (qos < 1 || qos > 6)
		return 0; /* stock warns and uses zero, not fatal assertion */
	for (index = qos >= 5 ? 5 : 1; index < 7; index++)
		sum += READ_ONCE(rq->android_oem_data1[7 + index]);
	return min(sum, READ_ONCE(rq->cpu_capacity_orig));
}

/* +471e40..4724e8. QoS>=5 on limited source CPUs restricts allowed mask;
 * passed as genuine live owner mask, not guessed core mask. */
static inline struct rq *x80_stock_svp_find_target(struct rq *source,
		struct task_struct *p, u8 cpu_qos, const struct cpumask *allowed,
		int *best_spare, unsigned long *source_spare)
{
	struct rq *best = NULL, *last = NULL, *target;
	unsigned long cap, load;
	int cpu, spare;
	u8 qos = cpu_qos ? x80_stock_task_read_u8(p, X80_QOS_LAST_ACCOUNTED) : 0;

	*best_spare = -1;
	cap = READ_ONCE(source->cpu_capacity_orig);
	*source_spare = cap - x80_stock_qos_balance_load(source, qos);
	for_each_possible_cpu(cpu) {
		if (cpu == cpu_of(source) || !cpumask_test_cpu(cpu, allowed) ||
		    !cpu_active(cpu))
			continue;
		target = cpu_rq(cpu);
		raw_spin_lock(&target->lock);
		if (!cpu_qos) {
			if (target->rt.rt_nr_running ||
			    READ_ONCE(*x80_stock_rq_shared_count(target))) {
				raw_spin_unlock(&target->lock);
				continue;
			}
			raw_spin_unlock(&target->lock);
			return target;
		}
		cap = READ_ONCE(target->cpu_capacity_orig);
		load = x80_stock_qos_balance_load(target, qos);
		spare = (s32)(cap - load);
		last = target;
		if (!best && !target->rt.rt_nr_running) {
			best = target;
			*best_spare = spare;
		} else if (*best_spare < spare && !target->rt.rt_nr_running) {
			best = target;
			*best_spare = spare;
		}
		raw_spin_unlock(&target->lock);
	}
	return best ? best : last;
}

/* +472004..47204c and +472554..47259c. Under source rq lock after target
 * discovery, task may exit/migrate while lock dropped: revalidate all three.
 * Fair cache-hot and CFS next/last buddy tests are additional callback gates. */
static inline bool x80_stock_svp_still_on_source(struct rq *source,
		struct task_struct *p)
{
	struct list_head *cursor, *node = x80_stock_task_shared_node(p);

	lockdep_assert_held(&source->lock);
	if (READ_ONCE(p->on_cpu) || task_rq(p) != source)
		return false;
	list_for_each(cursor, x80_stock_rq_shared_head(source))
		if (cursor == node)
			return true;
	return false;
}
#endif
