/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_BALANCE_CALLBACK_H
#define _X80_STOCK_QOS_BALANCE_CALLBACK_H
/* Include in core.c AFTER real enqueue_task/dequeue_task/check_preempt_curr.
 * They remain genuine core helpers, not exported synthetic implementations. */
#include "x80_stock_qos_balance_owner.h"
extern struct cpumask rsc_cpu_bigcore;
/* Genuine core helper is defined later in core.c; no substitute body. */
static inline bool is_cpu_allowed(struct task_struct *p, int cpu);

static int x80_stock_do_svp_balance(void *arg)
{
	struct rq *source = arg, *target = NULL;
	struct task_struct *p;
	struct cpumask allowed;
	struct sched_domain *sd;
	bool common_domain = false;
	unsigned long source_spare = 0;
	int cpu = cpu_of(source), best_spare = -1;
	u8 cpu_qos = READ_ONCE(rsc_cpu_qos_enable), qos;
	bool detached = false, pair_locked = false;

	local_irq_disable();
	raw_spin_lock(&source->lock);
	p = READ_ONCE(per_cpu(svp_balance_task, cpu));
	if (!READ_ONCE(*x80_stock_svp_balance_busy(source)) ||
	    cpu != smp_processor_id() || !p)
		goto cancel;
	if (!cpu_qos && READ_ONCE(*x80_stock_rq_shared_count(source)) <= 1)
		goto cancel;
	qos = cpu_qos ? x80_stock_task_read_u8(p, X80_QOS_LAST_ACCOUNTED) : 0;
	/* Stock uses task cpus_mask (not cpus_ptr). High QoS on mask CPUs is
	 * restricted to the genuine bigcore mask, then cpu_online_mask. */
	cpumask_copy(&allowed, &p->cpus_mask);
	if (qos >= 5 && cpumask_test_cpu(cpu, &rsc_cpu_bigcore))
		cpumask_and(&allowed, &allowed, &rsc_cpu_bigcore);
	cpumask_and(&allowed, &allowed, cpu_online_mask);
	raw_spin_unlock(&source->lock);
	target = x80_stock_svp_find_target(source, p, cpu_qos, &allowed,
		&best_spare, &source_spare);
	/* Corrective final validation closes target-selection affinity/cpuset,
	 * hotplug and root-domain races. RCU protects root-domain/span lifetime;
	 * BOTH rq locks serialize affinity updates and queue transition. */
	if (!target) {
		raw_spin_lock(&source->lock);
		goto cancel;
	}
	rcu_read_lock();
	double_rq_lock(source, target);
	pair_locked = true;
	/* Conservative correction: detached/no-balance CPUs can share the default
	 * root domain. Require a real common scheduler domain under RCU too. */
	for_each_domain(cpu_of(target), sd) {
		if (cpumask_test_cpu(cpu_of(source), sched_domain_span(sd))) {
			common_domain = true;
			break;
		}
	}
	if (!common_domain || !cpu_active(cpu_of(source)) ||
	    target == source || !cpu_active(cpu_of(target)) ||
	    !is_cpu_allowed(p, cpu_of(target)) ||
	    source->rd != target->rd ||
	    !cpumask_test_cpu(cpu_of(target), source->rd->span) ||
	    p->sched_class != &fair_sched_class ||
	    !task_on_rq_queued(p) ||
	    (cpu_qos && best_spare <= (s32)source_spare) ||
	    !x80_stock_svp_still_on_source(source, p) ||
	    x80_stock_svp_task_cache_hot(source, p))
		goto cancel;
	if (p->sched_class == &fair_sched_class && p->policy != SCHED_IDLE &&
	    sched_feat(CACHE_HOT_BUDDY) && target->nr_running &&
	    (source->cfs.next == &p->se || source->cfs.last == &p->se))
		goto cancel;
	/* +47329c: on_rq=MIGRATING before and after genuine dequeue_task.
	 * Matching scheduler dequeue drops old shared-list ref and aggregates.
	 * The extra balance ref keeps p alive throughout the transfer gap. */
	WRITE_ONCE(p->on_rq, TASK_ON_RQ_MIGRATING);
	dequeue_task(source, p, 0);
	WRITE_ONCE(p->on_rq, TASK_ON_RQ_MIGRATING);
	set_task_cpu(p, cpu_of(target));
	enqueue_task(target, p, ENQUEUE_MIGRATED);
	WRITE_ONCE(p->on_rq, TASK_ON_RQ_QUEUED);
	check_preempt_curr(target, p, 0);
	detached = true;
 cancel:
	if (detached && cpu == 7 &&
	    (p->pid == READ_ONCE(rsc_first_candidate_pid) ||
	     p->pid == READ_ONCE(rsc_second_candidate_pid)))
		WRITE_ONCE(rsc_heavy_task_push_suc_count,
			READ_ONCE(rsc_heavy_task_push_suc_count) + 1);
	WRITE_ONCE(*x80_stock_svp_balance_busy(source), 0);
	WRITE_ONCE(per_cpu(svp_balance_task, cpu), NULL);
	if (pair_locked) {
		double_rq_unlock(source, target);
		rcu_read_unlock();
	} else {
		raw_spin_unlock(&source->lock);
	}
	local_irq_enable();
	if (p)
		put_task_struct(p);
	return 0;
}

/* Stock trigger rq lock is plain raw_spin_lock (caller IRQ context).
 * Corrective deviation: failed stop queue submission drains the balance
 * reference and busy state, because offline stopper does not call callback.
 * Successful queue task/lifetime and callback semantics are preserved. */
static inline void x80_stock_trigger_svp_balance(struct rq *rq, u8 sample)
{
	struct task_struct *p, *cursor;
	struct list_head *node;
	int qos, floor;

	if (!rq)
		return;
	raw_spin_lock(&rq->lock);
	p = x80_stock_svp_balance_candidate_locked(rq, sample);
	if (!p || READ_ONCE(p->on_cpu) || x80_stock_svp_task_cache_hot(rq, p))
		goto unlock;
	if (cpu_of(rq) == 7 &&
	    (p->pid == READ_ONCE(rsc_first_candidate_pid) ||
	     p->pid == READ_ONCE(rsc_second_candidate_pid))) {
		qos = get_task_qos(p);
		floor = min(qos, 4);
		list_for_each(node, &rq->cfs_tasks) {
			cursor = list_entry(node, struct task_struct, se.group_node);
			if (cursor != p && get_task_qos(cursor) >= floor) {
				WRITE_ONCE(rsc_heavy_task_push_cont_count,
					READ_ONCE(rsc_heavy_task_push_cont_count) + 1);
				goto queue;
			}
		}
		WRITE_ONCE(rsc_heavy_task_push_skip_count,
			READ_ONCE(rsc_heavy_task_push_skip_count) + 1);
		goto unlock;
	}
 queue:
	if (READ_ONCE(*x80_stock_svp_balance_busy(rq)))
		goto unlock;
	WRITE_ONCE(*x80_stock_svp_balance_busy(rq), 1);
	get_task_struct(p);
	WRITE_ONCE(per_cpu(svp_balance_task, cpu_of(rq)), p);
	/* Stock +4715e8..4715f8 clears list and done before queueing. */
	memset(x80_stock_svp_balance_work(rq), 0,
	       sizeof(*x80_stock_svp_balance_work(rq)));
	raw_spin_unlock(&rq->lock);
	if (!stop_one_cpu_nowait(cpu_of(rq), x80_stock_do_svp_balance, rq,
			x80_stock_svp_balance_work(rq))) {
		raw_spin_lock(&rq->lock);
		if (READ_ONCE(per_cpu(svp_balance_task, cpu_of(rq))) == p) {
			WRITE_ONCE(per_cpu(svp_balance_task, cpu_of(rq)), NULL);
			WRITE_ONCE(*x80_stock_svp_balance_busy(rq), 0);
		}
		raw_spin_unlock(&rq->lock);
		put_task_struct(p);
	}
	return;
 unlock:
	raw_spin_unlock(&rq->lock);
}
#endif
