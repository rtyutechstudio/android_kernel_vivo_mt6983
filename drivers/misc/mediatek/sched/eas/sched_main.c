// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/energy_model.h>
#include <linux/jump_label.h>
#include <trace/events/sched.h>
#include <trace/events/task.h>
#include <trace/hooks/sched.h>
#include <sched/sched.h>
#include "common.h"
#include "eas_plus.h"
#include "sched_sys_common.h"

#define CREATE_TRACE_POINTS
#include "eas_trace.h"

int mtk_sched_asym_cpucapacity  =  1;

static inline void sched_asym_cpucapacity_init(void)
{
	struct perf_domain *pd;
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;
	int pd_count = 0;

	rcu_read_lock();
	pd = rcu_dereference(rd->pd);
	for (; pd; pd = pd->next)
		pd_count++;
	rcu_read_unlock();
	if (pd_count <= 1)
		mtk_sched_asym_cpucapacity = 0;
}

static void sched_task_util_hook(void *data, struct sched_entity *se)
{
	if (trace_sched_task_util_enabled()) {
		struct task_struct *p;
		struct sched_avg *sa;

		if (!entity_is_task(se))
			return;

		p = container_of(se, struct task_struct, se);
		sa = &se->avg;

		trace_sched_task_util(p->pid,
				sa->util_avg, sa->util_est.enqueued, sa->util_est.ewma);
	}
}

static void sched_task_uclamp_hook(void *data, struct sched_entity *se)
{
	if (trace_sched_task_uclamp_enabled()) {
		struct task_struct *p;
		struct sched_avg *sa;
		struct util_est ue;
		struct uclamp_se *uc_min_req, *uc_max_req;
		unsigned long util;

		if (!entity_is_task(se))
			return;

		p = container_of(se, struct task_struct, se);
		sa = &se->avg;
		ue = READ_ONCE(se->avg.util_est);
		util = max(ue.ewma, ue.enqueued);
		util = max(util, READ_ONCE(se->avg.util_avg));
		uc_min_req = &p->uclamp_req[UCLAMP_MIN];
		uc_max_req = &p->uclamp_req[UCLAMP_MAX];

		trace_sched_task_uclamp(p->pid, util,
				p->uclamp[UCLAMP_MIN].active,
				p->uclamp[UCLAMP_MIN].value, p->uclamp[UCLAMP_MAX].value,
				uc_min_req->user_defined, uc_min_req->value,
				uc_max_req->user_defined, uc_max_req->value);
	}
}

static int enqueue;
static int dequeue;
static void sched_queue_task_hook(void *data, struct rq *rq, struct task_struct *p, int flags)
{
	int cpu = rq->cpu;
	int type = *(int *)data;
	if (trace_sched_queue_task_enabled()) {
		unsigned long util = READ_ONCE(rq->cfs.avg.util_avg);

		util = max_t(unsigned long, util,
			     READ_ONCE(rq->cfs.avg.util_est.enqueued));

		trace_sched_queue_task(cpu, p->pid, type, util,
				rq->uclamp[UCLAMP_MIN].value, rq->uclamp[UCLAMP_MAX].value,
				p->uclamp[UCLAMP_MIN].value, p->uclamp[UCLAMP_MAX].value);
	}

#if IS_ENABLED(CONFIG_MTK_CPUFREQ_SUGOV_EXT)
	spin_lock(&per_cpu(cpufreq_idle_cpu_lock, cpu));
	if ((type == dequeue) && dequeue_idle_cpu(cpu) && (flags & DEQUEUE_SLEEP))
		per_cpu(cpufreq_idle_cpu, cpu) = 1;
	else
		per_cpu(cpufreq_idle_cpu, cpu) = 0;
	spin_unlock(&per_cpu(cpufreq_idle_cpu_lock, cpu));
#endif
}

/* Read-only status: resident does not imply all registrations succeeded. */
static int registration_error;
static char *registration_failed_stage = "none";
static const struct kernel_param_ops registration_error_ops = {
	.get = param_get_int,
};
static const struct kernel_param_ops registration_stage_ops = {
	.get = param_get_charp,
};
module_param_cb(registration_error, &registration_error_ops,
		&registration_error, 0444);
MODULE_PARM_DESC(registration_error, "First boot-lifetime registration error");
module_param_cb(registration_failed_stage, &registration_stage_ops,
		&registration_failed_stage, 0444);
MODULE_PARM_DESC(registration_failed_stage, "First failed registration stage");

static void __init mtk_sched_registration_result(const char *stage, int ret)
{
	if (!ret)
		return;
	if (!registration_error) {
		registration_error = ret;
		registration_failed_stage = (char *)stage;
	}
	pr_crit("scheduler: boot-lifetime registration %s failed: %d; retaining module, DEGRADED\n",
		stage, ret);
}

#define MTK_REGISTER_HOOK(name, probe, data) \
	mtk_sched_registration_result(#name ":" #probe, \
		register_trace_##name(probe, data))

static void __init mtk_sched_trace_init(void)
{
	enqueue = 1;
	dequeue = -1;

	MTK_REGISTER_HOOK(android_rvh_enqueue_task, sched_queue_task_hook, &enqueue);
	MTK_REGISTER_HOOK(android_rvh_dequeue_task, sched_queue_task_hook, &dequeue);

	MTK_REGISTER_HOOK(pelt_se_tp, sched_task_util_hook, NULL);

	MTK_REGISTER_HOOK(pelt_se_tp, sched_task_uclamp_hook, NULL);
}

static int __init mtk_scheduler_init(void)
{
	int ret = 0;

	ret = init_sched_common_sysfs();
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_MTK_EAS)
	ret = init_sram_info();
	if (ret)
		goto cleanup_sysfs;

	ret = mtk_freq_limit_notifier_register();
	if (ret)
		goto cleanup_sram;
#endif

	/*
	 * Commit point: static_power may publish proc ops during driver probing
	 * and has no teardown API, even if driver registration later fails.
	 * Restricted vendor hooks likewise cannot be unregistered (see
	 * trace/hooks/vendor_hooks.h). From BEFORE this call, NEVER return an
	 * error: the module loader could free already published callbacks.
	 * This module deliberately has no module_exit and lives until reboot.
	 */
	ret = mtk_static_power_init();
	mtk_sched_registration_result("static_power", ret);

	sched_asym_cpucapacity_init();

#if IS_ENABLED(CONFIG_MTK_EAS)

	MTK_REGISTER_HOOK(android_rvh_find_busiest_group, mtk_find_busiest_group, NULL);

	MTK_REGISTER_HOOK(android_rvh_find_energy_efficient_cpu, mtk_find_energy_efficient_cpu, NULL);

	MTK_REGISTER_HOOK(android_rvh_cpu_overutilized, mtk_cpu_overutilized, NULL);

	MTK_REGISTER_HOOK(android_rvh_tick_entry, mtk_tick_entry, NULL);

	MTK_REGISTER_HOOK(android_vh_set_wake_flags, mtk_set_wake_flags, NULL);

	MTK_REGISTER_HOOK(android_rvh_update_cpu_capacity, mtk_update_cpu_capacity, NULL);

#if IS_ENABLED(CONFIG_MTK_NEWIDLE_BALANCE)
	MTK_REGISTER_HOOK(android_rvh_sched_newidle_balance, mtk_sched_newidle_balance, NULL);
#endif
#endif

	MTK_REGISTER_HOOK(android_vh_scheduler_tick, hook_scheduler_tick, NULL);

	MTK_REGISTER_HOOK(android_rvh_after_enqueue_task, mtk_hook_after_enqueue_task, NULL);

#if IS_ENABLED(CONFIG_MTK_SCHED_BIG_TASK_ROTATE)
	MTK_REGISTER_HOOK(android_rvh_new_task_stats, rotat_task_stats, NULL);

	MTK_REGISTER_HOOK(task_newtask, rotat_task_newtask, NULL);
#endif
	MTK_REGISTER_HOOK(android_rvh_select_task_rq_rt, mtk_select_task_rq_rt, NULL);

	mtk_sched_trace_init();

	if (registration_error)
		pr_crit("scheduler: DEGRADED boot-lifetime init; first failure %s: %d; reboot required to retry\n",
			registration_failed_stage, registration_error);
	else
		pr_info("scheduler: boot-lifetime registrations complete; unload unsupported\n");
	return 0;

#if IS_ENABLED(CONFIG_MTK_EAS)
cleanup_sram:
	cleanup_sram_info();
cleanup_sysfs:
#endif
	cleanup_sched_common_sysfs();
	pr_err("scheduler: preparation failed: %d; rolled back\n", ret);
	return ret;
}

module_init(mtk_scheduler_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek scheduler");
MODULE_AUTHOR("MediaTek Inc.");
