// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <sched/sched.h>
#include <sugov/cpufreq.h>

MODULE_LICENSE("GPL");
/*
 *  max_freq_scale:
 *	max cpu frequency limit
 *	range: 0~SCHED_CAPACITY_SCALE
 *  min_freq:
 *	min cpu frequency limit
 *	unit: frequency
 */
DEFINE_PER_CPU(unsigned long, max_freq_scale) = SCHED_CAPACITY_SCALE;
DEFINE_PER_CPU(unsigned long, min_freq) = 0;
DEFINE_PER_CPU(unsigned int, policy_id_for_cpu) = 0;

#if IS_ENABLED(CONFIG_MTK_EAS)
static struct notifier_block *freq_limit_max_notifier, *freq_limit_min_notifier;
static int policy_num;

static int freq_limit_max_notifier_call(struct notifier_block *nb,
					 unsigned long freq_limit_max, void *ptr)
{
	int i, cpu, policy_idx = nb - freq_limit_max_notifier;
	struct em_perf_domain *pd;

	if (policy_idx < 0 || policy_idx >= policy_num) {
		pr_info("freq_limit_max_notifier_call: policy_idx over-index\n");
		return -1;
	}

	for_each_possible_cpu(cpu) {
		if (per_cpu(policy_id_for_cpu, cpu) == policy_idx) {
			pd = em_cpu_get(cpu);
			if (pd)
				break;
		}
	}

	if (!pd) {
		pr_info("policy_idx=%d: em_cpu_get failed\n", policy_idx);
		return -1;
	}

	for (i = 0; i < pd->nr_perf_states; i++) {
		if (pd->table[pd->nr_perf_states - i - 1].frequency <= freq_limit_max)
			break;
	}

	for_each_possible_cpu(cpu) {
		if (per_cpu(policy_id_for_cpu, cpu) == policy_idx) {
			per_cpu(max_freq_scale, cpu) = pd_get_opp_capacity(cpu, i);
		}
	}

	return 0;
}

static int freq_limit_min_notifier_call(struct notifier_block *nb,
					 unsigned long freq_limit_min, void *ptr)
{
	int cpu, policy_idx = nb - freq_limit_min_notifier;

	if (policy_idx < 0 || policy_idx >= policy_num) {
		pr_info("freq_limit_min_notifier_call: policy_idx over-index\n");
		return -1;
	}

	for_each_possible_cpu(cpu) {
		if (per_cpu(policy_id_for_cpu, cpu) == policy_idx)
			per_cpu(min_freq, cpu) = freq_limit_min;
	}

	return 0;
}

/* Keep constraints alive until preparation is rolled back, or for boot life. */
static struct cpufreq_policy **freq_limit_policies;
static int notifier_pairs;

void mtk_freq_limit_notifier_unregister(void)
{
	int i;

	for (i = notifier_pairs - 1; i >= 0; i--) {
		freq_qos_remove_notifier(&freq_limit_policies[i]->constraints,
			FREQ_QOS_MIN, freq_limit_min_notifier + i);
		freq_qos_remove_notifier(&freq_limit_policies[i]->constraints,
			FREQ_QOS_MAX, freq_limit_max_notifier + i);
	}
	notifier_pairs = 0;
	for (i = 0; i < policy_num; i++)
		cpufreq_cpu_put(freq_limit_policies[i]);
	policy_num = 0;
	kfree(freq_limit_policies);
	kfree(freq_limit_min_notifier);
	kfree(freq_limit_max_notifier);
	freq_limit_policies = NULL;
	freq_limit_min_notifier = NULL;
	freq_limit_max_notifier = NULL;
}

int mtk_freq_limit_notifier_register(void)
{
	struct cpufreq_policy *policy;
	int cpu, cpu_idx, i, ret = -ENOMEM;

	freq_limit_policies = kcalloc(num_possible_cpus(),
				     sizeof(*freq_limit_policies), GFP_KERNEL);
	freq_limit_max_notifier = kcalloc(num_possible_cpus(),
				 sizeof(*freq_limit_max_notifier), GFP_KERNEL);
	freq_limit_min_notifier = kcalloc(num_possible_cpus(),
				 sizeof(*freq_limit_min_notifier), GFP_KERNEL);
	if (!freq_limit_policies || !freq_limit_max_notifier ||
	    !freq_limit_min_notifier)
		goto fail;

	/* Snapshot policies before publishing any notifier callbacks. */
	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		for_each_cpu(cpu_idx, policy->related_cpus)
			per_cpu(policy_id_for_cpu, cpu_idx) = policy_num;
		freq_limit_policies[policy_num++] = policy;
		cpu = cpumask_last(policy->related_cpus);
	}

	if (!policy_num) {
		ret = -ENODEV;
		goto fail;
	}

	for (i = 0; i < policy_num; i++) {
		policy = freq_limit_policies[i];
		freq_limit_max_notifier[i].notifier_call = freq_limit_max_notifier_call;
		freq_limit_min_notifier[i].notifier_call = freq_limit_min_notifier_call;
		ret = freq_qos_add_notifier(&policy->constraints, FREQ_QOS_MAX,
					  freq_limit_max_notifier + i);
		if (ret)
			goto fail;
		ret = freq_qos_add_notifier(&policy->constraints, FREQ_QOS_MIN,
					  freq_limit_min_notifier + i);
		if (ret) {
			freq_qos_remove_notifier(&policy->constraints, FREQ_QOS_MAX,
					 freq_limit_max_notifier + i);
			goto fail;
		}
		notifier_pairs++;
	}
	return 0;

fail:
	mtk_freq_limit_notifier_unregister();
	return ret;
}

void mtk_update_cpu_capacity(void *data, int cpu, unsigned long *capacity)
{
	unsigned long cap_ceiling;

	cap_ceiling = per_cpu(max_freq_scale, cpu);
	*capacity = min(cap_ceiling, *capacity);
}

#endif
