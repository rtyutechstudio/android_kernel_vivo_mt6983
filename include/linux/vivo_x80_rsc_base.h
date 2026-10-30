/* SPDX-License-Identifier: GPL-2.0-only */
/* Native source API; not an ABI certification for stock consumers. */
#ifndef _RSC_BASE_CANDIDATE_H
#define _RSC_BASE_CANDIDATE_H
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#define RSC_BASE_CLUSTERS 3
struct rsc_base_snapshot {
	u32 min, max, old, cur;
	bool valid;
};
extern struct kobject *rsc_root_dir;
extern cpumask_t rsc_cpu_littlecore, rsc_cpu_midcore;
extern cpumask_t cpus_paused_by_thermal;
extern struct cpufreq_freqs rsc_cpu_freqs[RSC_BASE_CLUSTERS];
extern unsigned int rsc_max_freq[RSC_BASE_CLUSTERS];
extern unsigned int *rsc_min_freq[RSC_BASE_CLUSTERS];
/* Direct cpufreq-core API. Disabled-tree stubs are in rsc_base_core.h. */
int rsc_base_core_init(void);
void rsc_base_policy_ready(struct cpufreq_policy *p);
void rsc_base_policy_update(struct cpufreq_policy *p, bool transition);
void rsc_base_policy_inactive(struct cpufreq_policy *p);
void rsc_base_policy_remove(struct cpufreq_policy *p);
/* This NEW API does not transfer any policy pointer to a reader. */
int rsc_base_read(unsigned int cluster, struct rsc_base_snapshot *out);
bool rsc_base_ready(void);
int rsc_base_read_all(struct rsc_base_snapshot out[RSC_BASE_CLUSTERS]);
int rsc_base_masks(cpumask_t *little, cpumask_t *mid, cpumask_t *big);
int rsc_base_thermal_read(cpumask_t *out);
struct kobject *rsc_base_kobject_get(void);
void rsc_base_kobject_put(struct kobject *root);
/* Source spelling intentionally distinct: stock chown CRC/CFI unresolved. */
int rsc_base_chown_attr(struct kobject *kobj, struct attribute *attr);
/* Caller MUST serialize action + commit using the one perf_ioctl mutex. */
int rsc_base_thermal_commit(unsigned int cpu, bool pause);
/* Deliberately no rsc_capacity_of_little substitute or guessed cluster ABI. */
#endif
