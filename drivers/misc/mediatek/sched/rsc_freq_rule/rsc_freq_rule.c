// SPDX-License-Identifier: GPL-2.0-only
/* Reconstructed from X80 stock rsc_freq_rule.ko; same-tree source, NOT binary ABI glue. */
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/vivo_x80_rsc_base.h>
#include "kernel/sched/sched.h"
#include <trace/hooks/sched.h>
#define CREATE_TRACE_POINTS
#include "rsc_freq_rule_trace.h"

/* Genuine same-tree owner API declarations; definitions reside with owners. */
extern int rsc_base_read_all(struct rsc_base_snapshot out[RSC_BASE_CLUSTERS]);
extern int rsc_base_masks(cpumask_t *little, cpumask_t *mid, cpumask_t *big);
extern int rsc_base_thermal_read(cpumask_t *out);
extern struct kobject *rsc_base_kobject_get(void);
extern void rsc_base_kobject_put(struct kobject *kobj);
extern unsigned long pd_get_opp_capacity(int cpu, int opp);
extern int pd_get_nr_caps(int cpu); /* true PD-owner count query */
extern bool is_panel_backlight_on(void);
#define STATES 64
#define PLATFORM_CPUS 8
struct rule_config {
	int mid1, little1, mid2, little2;
	unsigned long map[STATES];
	struct rcu_head rcu;
};
static struct rule_config __rcu *rules;
static DEFINE_MUTEX(control_lock);
static cpumask_t little_mask, mid_mask, big_mask;
static unsigned long mid_freq[STATES], little_freq[STATES], little_cap[STATES];
static unsigned int mid_count, little_count;
static unsigned long baseline_capacity;
/* Genuine policy-owned dynamic output; never a fabricated provider/global export. */
static unsigned long decision_capacity;
static bool disabled, initialized, publication_failed;
static unsigned int rfr_polling_ms = 256;
static bool little_loading_check = true;
static int polling_set(const char *value, const struct kernel_param *kp)
{
	unsigned int ms;
	int ret = kstrtouint(value, 0, &ms);
	if (ret) return ret;
	if (ms < 1 || ms > 60000) return -ERANGE;
	WRITE_ONCE(*(unsigned int *)kp->arg, ms);
	return 0;
}
static const struct kernel_param_ops polling_ops = {
	.set = polling_set, .get = param_get_uint,
};
module_param_cb(rfr_polling_ms, &polling_ops, &rfr_polling_ms, 0644);
module_param(little_loading_check, bool, 0644);
static unsigned int loading[PLATFORM_CPUS];
static u64 previous_wall[PLATFORM_CPUS], previous_idle[PLATFORM_CPUS];
static atomic64_t high_load_count, min_count, boost_count, rule_count;
static struct workqueue_struct *load_wq;
static struct hrtimer load_timer;
static struct kobject *root;
static void load_worker(struct work_struct *work);
static DECLARE_WORK(load_work, load_worker);

/* Lower bound, exactly count(freq < value), not nearest OPP or arbitrary rounding. */
static unsigned int freq_state(const unsigned long *table, unsigned int count,
			      unsigned long value)
{
	unsigned int i, state = 0;
	for (i = 0; i < count; i++)
		state += table[i] < value;
	return state;
}
static bool live_snapshot(struct rsc_base_snapshot *little,
			  struct rsc_base_snapshot *mid)
{
	struct rsc_base_snapshot all[RSC_BASE_CLUSTERS];
	if (rsc_base_read_all(all)) return false;
	*little = all[0]; *mid = all[1];
	return little->valid && mid->valid;
}
static unsigned int decide_frequency(unsigned int requested, unsigned int cpu)
{
	struct rsc_base_snapshot l = { }, m = { };
	cpumask_t thermal;
	struct rule_config *cfg;
	unsigned int mi, li, ci, chosen = requested, candidate = requested;
	unsigned long cap = baseline_capacity;
	if (READ_ONCE(disabled) || !smp_load_acquire(&initialized))
		goto out;
	if (!live_snapshot(&l, &m) || rsc_base_thermal_read(&thermal) ||
	    !cpumask_empty(&thermal))
		goto out;
	if (!cpumask_subset(&mid_mask, cpu_online_mask) && is_panel_backlight_on())
		goto out;
	if (READ_ONCE(little_loading_check) &&
	    READ_ONCE(loading[2]) + READ_ONCE(loading[3]) >= 180 &&
	    READ_ONCE(loading[4]) <= 65 && READ_ONCE(loading[5]) <= 65 &&
	    READ_ONCE(loading[6]) <= 65) {
		atomic64_inc(&high_load_count);
		goto out;
	}
	if (m.cur >= m.max)
		goto out;
	mi = freq_state(mid_freq, mid_count, m.cur);
	/* Stock traps at >=64; reject dynamically invalid input instead. */
	if (mi >= mid_count)
		goto out;
	rcu_read_lock();
	cfg = rcu_dereference(rules);
	li = cfg ? cfg->map[mi] : STATES;
	rcu_read_unlock();
	if (li >= little_count)
		goto out;
	candidate = little_freq[li];
	/* Stock gives the minimum priority even above requested; do NOT only-down clamp. */
	if (l.min > candidate) {
		chosen = l.min;
		atomic64_inc(&min_count);
	} else if ((m.min >> 8) >= 0xc35) {
		chosen = little_freq[20];
		if (chosen >= requested) { chosen = requested; goto out; }
		atomic64_inc(&boost_count);
	} else {
		if (candidate >= requested)
			goto out;
		chosen = candidate;
		atomic64_inc(&rule_count);
	}
	ci = freq_state(little_freq, little_count, chosen);
	if (ci >= little_count) { chosen = requested; goto out; }
	cap = little_cap[ci];
	trace_limit_next_freq(cpu, requested, l.min, candidate, chosen);
out:
	WRITE_ONCE(decision_capacity, cap);
	return chosen;
}

/* Observation only: source0=capacity callback; source1=tick after rq store.
 * A target CPU can be queried from several execution CPUs, hence cmpxchg rather
 * than an unsynchronized per-CPU timestamp. Clock/atomic work only while enabled.
 */
static atomic64_t capacity_sample_ns[2][PLATFORM_CPUS];
static bool capacity_sample_due(unsigned int source, int cpu)
{
	u64 now, prev;
	if (!trace_capacity_apply_enabled() || cpu < 0 || cpu >= PLATFORM_CPUS)
		return false;
	now = ktime_get_mono_fast_ns();
	prev = atomic64_read(&capacity_sample_ns[source][cpu]);
	if (prev && now - prev < 20 * NSEC_PER_MSEC)
		return false;
	return atomic64_cmpxchg(&capacity_sample_ns[source][cpu], prev, now) == prev;
}

/* Stock arithmetic unchanged. A sample contains values actually used, not re-reads.
 * No allocation, rq lock acquisition, printk, or policy lookup is added here.
 */
static bool capacity_apply(int cpu, unsigned long *capacity,
			   struct rsc_capacity_sample *sample)
{
	struct rsc_base_snapshot l, m;
	struct rule_config *cfg;
	struct rq *rq;
	unsigned int mi, li, min_state;
	unsigned long cap, irq, rt, dl, thermal, other, available, cap_in;
	if (READ_ONCE(disabled) || !smp_load_acquire(&initialized) ||
	    !cpumask_test_cpu(cpu, &little_mask) || !live_snapshot(&l, &m))
		return false;
	mi = freq_state(mid_freq, mid_count, m.cur);
	if (mi >= mid_count)
		return false;
	rcu_read_lock();
	cfg = rcu_dereference(rules);
	li = cfg ? cfg->map[mi] : STATES;
	rcu_read_unlock();
	min_state = freq_state(little_freq, little_count, l.min);
	li = max(li, min_state);
	if ((m.min >> 8) > 0xc34)
		li = max(li, 20U);
	if (li >= little_count)
		return false;
	cap = little_cap[li];
	rq = cpu_rq(cpu);
	irq = READ_ONCE(rq->avg_irq.util_avg);
	rt = READ_ONCE(rq->avg_rt.util_avg);
	dl = READ_ONCE(rq->avg_dl.util_avg);
	thermal = READ_ONCE(rq->avg_thermal.load_avg);
	other = rt + dl + thermal;
	if (cap <= irq || cap <= other)
		available = 1;
	else
		available = (cap - irq) * (cap - other) / cap;
	cap_in = *capacity;
	*capacity = min(cap_in, available);
	if (sample) {
		sample->cpu = cpu;
		sample->mid_state = mi;
		sample->little_state = li;
		sample->cap_in = cap_in;
		sample->cap_opp = cap;
		sample->irq = irq;
		sample->rt = rt;
		sample->dl = dl;
		sample->thermal = thermal;
		sample->available = available;
		sample->cap_out = *capacity;
	}
	return true;
}
static void capacity_hook(void *unused, int cpu, unsigned long *capacity)
{
	struct rsc_capacity_sample sample;
	bool observe = capacity_sample_due(0, cpu);
	if (capacity_apply(cpu, capacity, observe ? &sample : NULL) && observe) {
		sample.source = 0;
		sample.rq_capacity = READ_ONCE(cpu_rq(cpu)->cpu_capacity);
		trace_capacity_apply(&sample);
	}
}
static void tick_hook(void *unused, struct rq *rq)
{
	struct rsc_capacity_sample sample;
	unsigned long cap = rq->cpu_capacity;
	bool observe = capacity_sample_due(1, cpu_of(rq));
	bool applied = capacity_apply(cpu_of(rq), &cap, observe ? &sample : NULL);
	/* Exact same tick assignment as before; trace observes AFTER this store. */
	rq->cpu_capacity = cap;
	if (applied && observe) {
		sample.source = 1;
		sample.rq_capacity = READ_ONCE(rq->cpu_capacity);
		trace_capacity_apply(&sample);
	}
}
static void sugov_hook(void *unused, unsigned int *next, unsigned int cpu)
{
	if (cpumask_test_cpu(cpu, &little_mask))
		*next = decide_frequency(*next, cpu);
}
static enum hrtimer_restart load_timeout(struct hrtimer *timer)
{
	queue_work(load_wq, &load_work);
	return HRTIMER_NORESTART;
}
static void arm_timer(void)
{
	u64 ns = (u64)READ_ONCE(rfr_polling_ms) * NSEC_PER_MSEC;
	/* Safety difference: no zero/overflow polling interval. */
	if (!ns) ns = NSEC_PER_MSEC;
	hrtimer_start(&load_timer, ns_to_ktime(ns), HRTIMER_MODE_REL);
}
static unsigned int sample_load(u64 wall, u64 idle, u64 old_wall,
			       u64 old_idle, bool active)
{
	u64 dw = wall - old_wall, di = idle - old_idle;
	/* Rebaseline long freeze gaps: stock's u32 divisor otherwise truncates. */
	if (idle == -1ULL || wall < old_wall || idle < old_idle ||
	    !active || dw <= di || dw > U32_MAX || !dw)
		return 0;
	return div64_u64((dw - di) * 100, (u32)dw);
}
static void load_worker(struct work_struct *work)
{
	unsigned int cpu;
	for_each_possible_cpu(cpu) {
		u64 wall = 0, idle;
		unsigned int load;
		if (cpu >= PLATFORM_CPUS) continue;
		idle = get_cpu_idle_time(cpu, &wall, 1);
		load = sample_load(wall, idle, previous_wall[cpu],
				   previous_idle[cpu], cpu_active(cpu));
		previous_wall[cpu] = wall;
		previous_idle[cpu] = idle;
		WRITE_ONCE(loading[cpu], load);
	}
	/* control_lock closes disable/cancel vs worker rearm race. */
	mutex_lock(&control_lock);
	if (!disabled && initialized) arm_timer();
	mutex_unlock(&control_lock);
}
static int read_table(unsigned int cpu, unsigned long *out, unsigned int *count)
{
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *pos;
	unsigned int n = 0, i;
	int ret = 0;
	policy = cpufreq_cpu_get(cpu);
	if (!policy) return -EPROBE_DEFER;
	if (!policy->freq_table || policy->freq_table_sorted == CPUFREQ_TABLE_UNSORTED) {
		ret = -EINVAL; goto out;
	}
	cpufreq_for_each_valid_entry(pos, policy->freq_table) {
		if (n == STATES) { ret = -E2BIG; goto out; }
		out[n++] = pos->frequency;
	}
	if (!n) { ret = -EINVAL; goto out; }
	if (policy->freq_table_sorted == CPUFREQ_TABLE_SORTED_DESCENDING)
		for (i = 0; i < n / 2; i++) swap(out[i], out[n - 1 - i]);
	for (i = 1; i < n; i++)
		if (out[i] <= out[i - 1]) { ret = -EINVAL; goto out; }
	*count = n;
out:
	cpufreq_cpu_put(policy);
	return ret;
}
static bool valid_rule(int a, int b, int c, int d)
{
	return a >= 0 && b >= 0 && c >= 0 && d >= 0 && a <= c && b <= d &&
		a < mid_count && c < mid_count && b < little_count && d < little_count;
}
static struct rule_config *alloc_rules(int a, int b, int c, int d)
{
	struct rule_config *cfg;
	unsigned int i;
	if (!valid_rule(a, b, c, d)) return ERR_PTR(-EINVAL);
	cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg) return ERR_PTR(-ENOMEM);
	cfg->mid1 = a; cfg->little1 = b; cfg->mid2 = c; cfg->little2 = d;
	for (i = 0; i < mid_count; i++)
		cfg->map[i] = i < a ? b : i < c ? d : little_count - 1;
	return cfg;
}
static ssize_t rule_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	struct rule_config *cfg, *old;
	int values[4], i = 0, ret;
	char input[128], *cursor = input, *token;
	if (!count || count >= sizeof(input) || memchr(buf, '\0', count)) return -EINVAL;
	memcpy(input, buf, count); input[count] = '\0';
	while ((token = strsep(&cursor, " \t\n\r")) != NULL) {
		if (!*token) continue;
		if (i == 4) return -EINVAL;
		ret = kstrtoint(token, 10, &values[i++]);
		if (ret) return ret;
	}
	if (i != 4) return -EINVAL;
	cfg = alloc_rules(values[0], values[1], values[2], values[3]);
	if (IS_ERR(cfg)) return PTR_ERR(cfg);
	mutex_lock(&control_lock);
	if (!initialized) { mutex_unlock(&control_lock); kfree(cfg); return -ENODEV; }
	old = rcu_dereference_protected(rules, lockdep_is_held(&control_lock));
	rcu_assign_pointer(rules, cfg);
	mutex_unlock(&control_lock);
	kfree_rcu(old, rcu);
	return count;
}
static ssize_t rule_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct rule_config *cfg;
	unsigned int i;
	ssize_t n = 0;
	if (!smp_load_acquire(&initialized)) return 0;
	rcu_read_lock(); cfg = rcu_dereference(rules);
	for (i = 0; i < mid_count && n < PAGE_SIZE; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n,
			"mid freq %7lu kHz -> little core max freq %7lu kHz\n",
			mid_freq[i], little_freq[cfg->map[i]]);
	n += scnprintf(buf + n, PAGE_SIZE - n,
		"mid_threshold1 %7lu kHz -> little_limit1 %7lu kHz\n"
		"mid_threshold2 %7lu kHz -> little_limit2 %7lu kHz\n",
		mid_freq[cfg->mid1], little_freq[cfg->little1],
		mid_freq[cfg->mid2], little_freq[cfg->little2]);
	rcu_read_unlock(); return n;
}
static ssize_t debug_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int value, ret = 0;
	ret = kstrtoint(buf, 10, &value);
	if (ret) return ret;
	if (value != 0 && value != 1) return -EINVAL;
	mutex_lock(&control_lock);
	if (!initialized || publication_failed) ret = -ENODEV;
	else if (!!value == !disabled) ret = -EINVAL;
	else {
		WRITE_ONCE(disabled, !value);
		if (value) arm_timer();
		else hrtimer_cancel(&load_timer);
	}
	mutex_unlock(&control_lock);
	return ret ? ret : count;
}
static ssize_t debug_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int cpu;
	ssize_t n = 0;
	n += scnprintf(buf + n, PAGE_SIZE - n,
		"rsc freq limit rule enabled:     %d\n"
		"limit to scaling min freq cnt:   %lld\n"
		"limit to rsc rule boost cnt:     %lld\n"
		"limit to rsc rule freq cnt:      %lld\n"
		"skip limit due to high load cnt: %lld\n"
		"cpu load period: %u ms\n",
		!READ_ONCE(disabled), atomic64_read(&min_count), atomic64_read(&boost_count),
		atomic64_read(&rule_count), atomic64_read(&high_load_count),
		READ_ONCE(rfr_polling_ms));
	for (cpu = 0; cpu < PLATFORM_CPUS; cpu++)
		n += scnprintf(buf + n, PAGE_SIZE - n, "cpu %d loading : %3u%%\n",
			cpu, READ_ONCE(loading[cpu]));
	n += scnprintf(buf + n, PAGE_SIZE - n, "panel display: %s\n",
		is_panel_backlight_on() ? "enabled" : "disabled");
	return n;
}
static struct kobj_attribute rule_attr = __ATTR(rsc_freq_rule, 0660, rule_show, rule_store);
static struct kobj_attribute debug_attr = __ATTR(rsc_freq_debug, 0660, debug_show, debug_store);
static int __init freq_rule_init(void)
{
	struct rule_config *cfg;
	unsigned int i;
	int ret;
	cpumask_t expected;
	if (!rsc_base_ready()) return -EPROBE_DEFER;
	ret = rsc_base_masks(&little_mask, &mid_mask, &big_mask);
	if (ret) return ret;
	/* Hard-coded load thresholds/OPP20 are stock X80 policy; validate the platform. */
	cpumask_clear(&expected);
	for (i = 0; i < 4; i++) cpumask_set_cpu(i, &expected);
	if (!cpumask_equal(&little_mask, &expected)) return -EINVAL;
	cpumask_clear(&expected);
	for (i = 4; i < 7; i++) cpumask_set_cpu(i, &expected);
	if (!cpumask_equal(&mid_mask, &expected)) return -EINVAL;
	cpumask_clear(&expected); cpumask_set_cpu(7, &expected);
	if (!cpumask_equal(&big_mask, &expected) || num_possible_cpus() != 8) return -EINVAL;
	ret = read_table(4, mid_freq, &mid_count);
	if (ret) return ret;
	ret = read_table(0, little_freq, &little_count);
	if (ret) return ret;
	if (little_count <= 20 || pd_get_nr_caps(0) != little_count) return -EINVAL;
	cfg = alloc_rules(17, 16, 25, 20);
	if (IS_ERR(cfg)) return PTR_ERR(cfg);
	for (i = 0; i < little_count; i++) {
		little_cap[i] = pd_get_opp_capacity(0, little_count - 1 - i);
		if (!little_cap[i] || little_cap[i] > SCHED_CAPACITY_SCALE ||
		    (i && little_cap[i] <= little_cap[i - 1])) { ret = -EINVAL; goto free_cfg; }
	}
	baseline_capacity = cpu_rq(0)->cpu_capacity_orig;
	decision_capacity = baseline_capacity;
	load_wq = alloc_ordered_workqueue("rfr_cpu_load_wq", WQ_FREEZABLE | WQ_MEM_RECLAIM);
	if (!load_wq) { ret = -ENOMEM; goto free_cfg; }
	hrtimer_init(&load_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	load_timer.function = load_timeout;
	root = rsc_base_kobject_get();
	if (!root) { ret = -ENODEV; goto destroy_queue; }
	ret = sysfs_create_file(root, &rule_attr.attr);
	if (ret) goto put_root;
	ret = rsc_base_chown_attr(root, &rule_attr.attr);
	if (ret) goto remove_rule;
	ret = sysfs_create_file(root, &debug_attr.attr);
	if (ret) goto remove_rule;
	ret = rsc_base_chown_attr(root, &debug_attr.attr);
	if (ret) goto remove_debug;
	rcu_assign_pointer(rules, cfg);
	/* Commit: a successful restricted registration is non-unregisterable. */
	ret = register_trace_android_rvh_update_cpu_capacity(capacity_hook, NULL);
	if (ret) goto clear_rules;
	ret = register_trace_android_rvh_tick_entry(tick_hook, NULL);
	if (ret) goto resident_failure;
	ret = register_trace_android_vh_sugov_update_shared(sugov_hook, NULL);
	if (ret) goto resident_failure;
	smp_store_release(&initialized, true);
	arm_timer();
	pr_info("rsc_rule: genuine source consumer initialized\n");
	return 0;
resident_failure:
	/* Module init must NOT return error and free text with restricted hook alive. */
	WRITE_ONCE(disabled, true);
	WRITE_ONCE(publication_failed, true);
	pr_err("rsc_rule: partial hook publication %d, retained disabled until reboot\n", ret);
	return 0;
clear_rules:
	RCU_INIT_POINTER(rules, NULL);
remove_debug:
	sysfs_remove_file(root, &debug_attr.attr);
remove_rule:
	sysfs_remove_file(root, &rule_attr.attr);
put_root:
	rsc_base_kobject_put(root);
destroy_queue:
	destroy_workqueue(load_wq);
free_cfg:
	kfree(cfg);
	return ret;
}
/* No module_exit: restricted callback publication requires reboot lifetime. */
module_init(freq_rule_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("X80 reconstructed genuine RSC frequency rule consumer");
