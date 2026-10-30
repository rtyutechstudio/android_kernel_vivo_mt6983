/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RSC_BASE_CORE_H
#define _RSC_BASE_CORE_H
#include <linux/cpufreq.h>
#if IS_ENABLED(CONFIG_VIVO_X80_RSC_BASE)
int rsc_base_core_init(void);
void rsc_base_policy_ready(struct cpufreq_policy *p);
void rsc_base_policy_update(struct cpufreq_policy *p, bool transition);
void rsc_base_policy_inactive(struct cpufreq_policy *p);
void rsc_base_policy_remove(struct cpufreq_policy *p);
#else
static inline int rsc_base_core_init(void) { return 0; }
static inline void rsc_base_policy_ready(struct cpufreq_policy *p) {}
static inline void rsc_base_policy_update(struct cpufreq_policy *p, bool t) {}
static inline void rsc_base_policy_inactive(struct cpufreq_policy *p) {}
static inline void rsc_base_policy_remove(struct cpufreq_policy *p) {}
#endif
#endif
