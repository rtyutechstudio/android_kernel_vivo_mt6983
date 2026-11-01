// SPDX-License-Identifier: GPL-2.0-only
/* Native X80 base owner, not complete stock RSC parity.
 * No scheduler changes, no QoS requests, no core_ctl references.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernfs.h>
#include <linux/uidgid.h>
#include <linux/sched/topology.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/vivo_x80_rsc_base.h>

/* Stock Image has eight bytes of zero-initialized debug storage; all proven
 * show/store/mtk_fpsgo accesses are to its low 32 bits. Keep the full word, not
 * a pointer, and preserve high bits on low-word sysfs updates. The exact stock
 * typedef/CRC is NOT certified by this semantic restoration candidate.
 */
unsigned long rsc_debug;
EXPORT_SYMBOL_GPL(rsc_debug);
static DEFINE_MUTEX(rsc_debug_lock);

struct kobject *rsc_root_dir;
struct cpumask rsc_cpu_littlecore, rsc_cpu_midcore, cpus_paused_by_thermal;
struct cpumask rsc_cpu_bigcore, rsc_cpu_maxcore;
/* Scalar capacity override: real stock freq-rule consumer writes this.
 * Do not refresh from arch capacity during policy update/topology replay.
 * Remaining core RT/scheduler readers must be restored separately.
 */
unsigned long rsc_capacity_of_little;
EXPORT_SYMBOL(rsc_root_dir);
EXPORT_SYMBOL_GPL(rsc_cpu_littlecore);
EXPORT_SYMBOL_GPL(rsc_cpu_midcore);
EXPORT_SYMBOL_GPL(rsc_cpu_bigcore);
EXPORT_SYMBOL_GPL(rsc_cpu_maxcore);
EXPORT_SYMBOL_GPL(rsc_capacity_of_little);
/* Official perf_ioctl commits this same mask only after real pause success. */
EXPORT_SYMBOL_GPL(cpus_paused_by_thermal);
struct cpufreq_freqs rsc_cpu_freqs[3];
unsigned int rsc_max_freq[3];
static unsigned int live_min[3];
unsigned int *rsc_min_freq[3] = { &live_min[0], &live_min[1], &live_min[2] };
/* Stock min pointees are effective policy->min snapshots, not QoS requests.
 * The permanent targets preserve consumer read lifetime across detach.
 */
EXPORT_SYMBOL_GPL(rsc_min_freq);
EXPORT_SYMBOL_GPL(rsc_max_freq);
/* All 237 official modules audited: only rsc_freq_rule imports this object,
 * and both access sites load its middle-cluster .new scalar (array+0x24).
 * They do not read or retain its native .policy pointer.
 */
EXPORT_SYMBOL_GPL(rsc_cpu_freqs);
/* Static targets live for kernel lifetime, including detached policies. */

static DEFINE_RAW_SPINLOCK(state_lock);
struct policy_slot {
 struct cpufreq_policy *owner; /* identity only; never async dereferenced */
 cpumask_t mask;
 u32 min, max, old, cur;
 bool active;
};
static struct policy_slot slots[3];
static int cluster_slot[3] = { -1, -1, -1 };
static bool armed, topology_valid, overflow;

static bool ready_locked(void)
{
 int i;
 if (!armed || !topology_valid || overflow)
  return false;
 for (i = 0; i < 3; ++i)
  if (cluster_slot[i] < 0 || !slots[cluster_slot[i]].active ||
      !slots[cluster_slot[i]].min || !slots[cluster_slot[i]].cur ||
      slots[cluster_slot[i]].min > slots[cluster_slot[i]].max)
   return false;
 return true;
}
bool rsc_base_ready(void)
{
 unsigned long f;
 bool ready;
 raw_spin_lock_irqsave(&state_lock, f);
 ready = ready_locked();
 raw_spin_unlock_irqrestore(&state_lock, f);
 return ready;
}
EXPORT_SYMBOL_GPL(rsc_base_ready);
int rsc_base_read(unsigned int i, struct rsc_base_snapshot *out)
{
 unsigned long f;
 if (i >= 3 || !out)
  return -EINVAL;
 raw_spin_lock_irqsave(&state_lock, f);
 if (!ready_locked()) {
  raw_spin_unlock_irqrestore(&state_lock, f);
  return -ENODATA;
 }
 *out = (struct rsc_base_snapshot) {
  .min = live_min[i], .max = rsc_max_freq[i],
  .old = rsc_cpu_freqs[i].old, .cur = rsc_cpu_freqs[i].new,
  .valid = true,
 };
 raw_spin_unlock_irqrestore(&state_lock, f);
 return 0;
}
EXPORT_SYMBOL_GPL(rsc_base_read);
/* New same-tree APIs: no compatibility pointers escape. */
int rsc_base_read_all(struct rsc_base_snapshot out[RSC_BASE_CLUSTERS])
{
 unsigned long f;
 int i;
 if (!out)
  return -EINVAL;
 raw_spin_lock_irqsave(&state_lock, f);
 if (!ready_locked()) {
  raw_spin_unlock_irqrestore(&state_lock, f);
  return -ENODATA;
 }
 for (i = 0; i < RSC_BASE_CLUSTERS; i++)
  out[i] = (struct rsc_base_snapshot) {
   .min = live_min[i], .max = rsc_max_freq[i],
   .old = rsc_cpu_freqs[i].old, .cur = rsc_cpu_freqs[i].new,
   .valid = true,
  };
 raw_spin_unlock_irqrestore(&state_lock, f);
 return 0;
}
EXPORT_SYMBOL_GPL(rsc_base_read_all);

int rsc_base_masks(cpumask_t *little, cpumask_t *mid, cpumask_t *big)
{
 unsigned long f;
 if (!little || !mid || !big)
  return -EINVAL;
 raw_spin_lock_irqsave(&state_lock, f);
 if (!ready_locked()) {
  raw_spin_unlock_irqrestore(&state_lock, f);
  return -ENODATA;
 }
 cpumask_copy(little, &slots[cluster_slot[0]].mask);
 cpumask_copy(mid, &slots[cluster_slot[1]].mask);
 cpumask_copy(big, &slots[cluster_slot[2]].mask);
 raw_spin_unlock_irqrestore(&state_lock, f);
 return 0;
}
EXPORT_SYMBOL_GPL(rsc_base_masks);

int rsc_base_thermal_read(cpumask_t *out)
{
 unsigned long f;
 if (!out)
  return -EINVAL;
 raw_spin_lock_irqsave(&state_lock, f);
 if (!ready_locked()) {
  raw_spin_unlock_irqrestore(&state_lock, f);
  return -ENODATA;
 }
 /* nr_cpumask_bits <= NR_CPUS32 fits one native word on this ARM64 target.
  * Matches atomic successful-commit writer; no action-in-progress guarantee.
  */
 BUILD_BUG_ON(NR_CPUS > BITS_PER_LONG);
 cpumask_bits(out)[0] = READ_ONCE(cpumask_bits(&cpus_paused_by_thermal)[0]);
 raw_spin_unlock_irqrestore(&state_lock, f);
 return 0;
}
EXPORT_SYMBOL_GPL(rsc_base_thermal_read);

struct kobject *rsc_base_kobject_get(void)
{
 struct kobject *root = NULL;
 unsigned long f;
 raw_spin_lock_irqsave(&state_lock, f);
 /* Root is permanent after armed. Caller receives a counted reference. */
 if (ready_locked() && rsc_root_dir)
  root = kobject_get(rsc_root_dir);
 raw_spin_unlock_irqrestore(&state_lock, f);
 return root;
}
EXPORT_SYMBOL_GPL(rsc_base_kobject_get);

void rsc_base_kobject_put(struct kobject *root)
{
 kobject_put(root);
}
EXPORT_SYMBOL_GPL(rsc_base_kobject_put);

static int find_owner(struct cpufreq_policy *p)
{
 int i;
 for (i = 0; i < 3; ++i)
  if (slots[i].owner == p)
   return i;
 return -1;
}
static void copy_policy(struct policy_slot *s, struct cpufreq_policy *p,
                        bool transition)
{
 u32 cur = READ_ONCE(p->cur);
 s->min = READ_ONCE(p->min);
 s->max = READ_ONCE(p->max);
 if (transition || !s->active) {
  s->old = s->active ? s->cur : cur;
  s->cur = cur;
 }
}
static void publish(int c, int i)
{
 struct policy_slot *s = &slots[i];
 WRITE_ONCE(live_min[c], s->min);
 WRITE_ONCE(rsc_max_freq[c], s->max);
 WRITE_ONCE(rsc_cpu_freqs[c].old, s->old);
 WRITE_ONCE(rsc_cpu_freqs[c].new, s->cur);
 WRITE_ONCE(rsc_cpu_freqs[c].policy, s->active ? s->owner : NULL);
}
/* Run only after three complete READY snapshots. No other owner dereference. */
static void replay_topology(void)
{
 cpumask_t covered, masks[3];
 unsigned long caps[3] = { 0, 0, 0 }, cap, tmp;
  unsigned long stock_threshold = 0;
  unsigned int stock_records = 0;
 int i, j, cpu, n = 0, mapping[3];
 topology_valid = false;
 cpumask_clear(&covered);
 for (i = 0; i < 3; ++i) {
  if (!slots[i].owner || !slots[i].active)
   return; /* early first/second READY is pending, not topology failure */
  if (!slots[i].min || slots[i].min > slots[i].max || !slots[i].cur ||
      cpumask_empty(&slots[i].mask) ||
      cpumask_intersects(&covered, &slots[i].mask))
   return;
  cpumask_or(&covered, &covered, &slots[i].mask);
 }
 if (!cpumask_equal(&covered, cpu_possible_mask))
  return;
 for_each_possible_cpu(cpu) {
  cap = arch_scale_cpu_capacity(cpu);
  if (!cap)
   return;
  /* Stock second strict record maximum in possible CPU order. */
  if (stock_records <= 1 && cap > stock_threshold) {
   stock_records++;
   stock_threshold = cap;
  }
  for (i = 0; i < n && caps[i] != cap; ++i) {}
  if (i == n) {
   if (n == 3)
    return;
   caps[n++] = cap;
  }
 }
 if (n != 3)
  return;
 for (i = 0; i < 3; ++i)
  for (j = i + 1; j < 3; ++j)
   if (caps[i] > caps[j]) {
    tmp = caps[i]; caps[i] = caps[j]; caps[j] = tmp;
   }
 /* Preserve native three-class API only on stock-supported CPU order. */
 if (stock_threshold != caps[1])
  return;
 for (i = 0; i < 3; ++i)
  cpumask_clear(&masks[i]);
 for_each_possible_cpu(cpu)
  for (i = 0; i < 3; ++i)
   if (arch_scale_cpu_capacity(cpu) == caps[i])
    cpumask_set_cpu(cpu, &masks[i]);
 for (i = 0; i < 3; ++i) {
  for (j = 0; j < 3; ++j)
   if (cpumask_equal(&masks[i], &slots[j].mask))
    break;
  if (j == 3)
   return;
  mapping[i] = j;
 }
 /* One locked publication, replaying scalars copied while owners were alive. */
 cpumask_copy(&rsc_cpu_littlecore, &masks[0]);
 cpumask_copy(&rsc_cpu_midcore, &masks[1]);
 /* Stock big is mid union max; native API big remains highest cluster. */
 cpumask_or(&rsc_cpu_bigcore, &masks[1], &masks[2]);
 cpumask_copy(&rsc_cpu_maxcore, &masks[2]);
 for (i = 0; i < 3; ++i) {
  cluster_slot[i] = mapping[i];
  publish(i, mapping[i]);
 }
 topology_valid = true;
}
void rsc_base_policy_ready(struct cpufreq_policy *p)
{
 unsigned long f;
 int i;
 raw_spin_lock_irqsave(&state_lock, f);
 if (!armed)
  goto out;
 i = find_owner(p);
 if (i < 0) {
  for (i = 0; i < 3 && slots[i].owner; ++i) {}
  if (i == 3) {
   overflow = true; /* unsupported fourth owner: fail closed until reboot */
   topology_valid = false;
   goto out;
  }
  slots[i].owner = p;
 }
 cpumask_copy(&slots[i].mask, p->related_cpus);
 copy_policy(&slots[i], p, true);
 slots[i].active = true;
 replay_topology();
out:
 raw_spin_unlock_irqrestore(&state_lock, f);
}
void rsc_base_policy_update(struct cpufreq_policy *p, bool transition)
{
 unsigned long f;
 int i, c;
 raw_spin_lock_irqsave(&state_lock, f);
 i = find_owner(p);
 if (!armed || i < 0 || !slots[i].active)
  goto out; /* never attach/revive from an update */
 copy_policy(&slots[i], p, transition);
 if (topology_valid)
  for (c = 0; c < 3; ++c)
   if (cluster_slot[c] == i)
    publish(c, i);
out:
 raw_spin_unlock_irqrestore(&state_lock, f);
}
static void deactivate(struct cpufreq_policy *p, bool remove)
{
 unsigned long f;
 int i, c;
 raw_spin_lock_irqsave(&state_lock, f);
 i = find_owner(p);
 if (i >= 0) {
  slots[i].active = false;
  for (c = 0; c < 3; ++c)
   if (cluster_slot[c] == i)
    WRITE_ONCE(rsc_cpu_freqs[c].policy, NULL);
  if (remove)
   slots[i].owner = NULL;
 }
 raw_spin_unlock_irqrestore(&state_lock, f);
}
void rsc_base_policy_inactive(struct cpufreq_policy *p) { deactivate(p, false); }
void rsc_base_policy_remove(struct cpufreq_policy *p) { deactivate(p, true); }

int rsc_base_chown_attr(struct kobject *kobj, struct attribute *attr)
{
	struct kernfs_node *kn;
	struct iattr ia = { .ia_valid = ATTR_UID | ATTR_GID,
		.ia_uid = KUIDT_INIT(1000), .ia_gid = KGIDT_INIT(1000) };
	int ret;
	/* Caller owns kobject/attribute lifetime; do not race attribute removal. */
	if (!kobj || !kobj->sd || !attr || !attr->name)
		return -EINVAL;
	kn = kernfs_find_and_get_ns(kobj->sd, attr->name, NULL);
	if (!kn)
		return -ENOENT;
	ret = kernfs_setattr(kn, &ia);
	kernfs_put(kn);
	return ret;
}
EXPORT_SYMBOL_GPL(rsc_base_chown_attr);

/* Shipped freq-rule init calls this directly after creating each sysfs file.
 * Both arguments have measured stock/current field offsets. The wrapper
 * performs the real UID/GID1000 mutation and balances the kernfs reference.
 * Exact natural genksyms CRC remains a separate unresolved ABI issue.
 */
int rsc_chown_to_system(struct kobject *kobj, struct attribute *attr)
{
 return rsc_base_chown_attr(kobj, attr);
}
EXPORT_SYMBOL_GPL(rsc_chown_to_system);

int rsc_base_thermal_commit(unsigned int cpu, bool pause)
{
	if (cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return -EINVAL;
	/* Provider makes NO call into core_ctl. Single writer mutex is in ioctl. */
	if (pause)
		set_bit(cpu, cpumask_bits(&cpus_paused_by_thermal));
	else
		clear_bit(cpu, cpumask_bits(&cpus_paused_by_thermal));
	return 0;
}
EXPORT_SYMBOL_GPL(rsc_base_thermal_commit);

static ssize_t ready_show(struct kobject *kobj, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", rsc_base_ready());
}
/* Copy one coherent base-owned snapshot; never expose policy pointers. */
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *a, char *buf)
{
 struct policy_slot copy[3];
 unsigned long f;
 int i, len = 0;
 raw_spin_lock_irqsave(&state_lock, f);
 if (!ready_locked()) {
  raw_spin_unlock_irqrestore(&state_lock, f);
  return -ENODATA;
 }
 for (i = 0; i < 3; i++)
  copy[i] = slots[cluster_slot[i]];
 raw_spin_unlock_irqrestore(&state_lock, f);
 for (i = 0; i < 3; i++)
  len += sysfs_emit_at(buf, len,
   "cluster=%d cpus=%*pbl min=%u max=%u old=%u cur=%u\n",
   i, cpumask_pr_args(&copy[i].mask), copy[i].min, copy[i].max,
   copy[i].old, copy[i].cur);
 return len;
}
static ssize_t thermal_mask_show(struct kobject *kobj,
                                struct kobj_attribute *a, char *buf)
{
 cpumask_t mask;
 unsigned int i;
 /* Atomic successful-commit bits; not a transactional physical-action view. */
 for (i = 0; i < BITS_TO_LONGS(NR_CPUS); i++)
  cpumask_bits(&mask)[i] = READ_ONCE(cpumask_bits(&cpus_paused_by_thermal)[i]);
 return sysfs_emit(buf, "%*pbl\n", cpumask_pr_args(&mask));
}
static struct kobj_attribute thermal_mask_attr = __ATTR_RO(thermal_mask);

static struct kobj_attribute state_attr = __ATTR_RO(state);

/* Stock /sys/rsc/debug is 0640, prints hexadecimal and parses with %x.
 * This is actual writable control state: the unchanged FPSGO consumer tests
 * bit21 (rsc_debug+2 bit5) in fpsgo_get_tgid_and_mark. It is not a ready gate
 * and must not be used to turn off the underlying task mark/CPU CSS policy.
 */
static ssize_t debug_show(struct kobject *kobj, struct kobj_attribute *a,
                          char *buf)
{
 return sysfs_emit(buf, "debug: %x\n", (unsigned int)READ_ONCE(rsc_debug));
}

static ssize_t debug_store(struct kobject *kobj, struct kobj_attribute *a,
                           const char *buf, size_t count)
{
 unsigned int value;
 unsigned long old;

 if (sscanf(buf, "%x", &value) != 1)
  return -EINVAL;
 mutex_lock(&rsc_debug_lock);
 old = READ_ONCE(rsc_debug);
 WRITE_ONCE(rsc_debug, (old & ~0xffffffffUL) | value);
 mutex_unlock(&rsc_debug_lock);
 return count;
}
static struct kobj_attribute debug_attr = __ATTR(debug, 0640,
                                                debug_show, debug_store);

static struct kobj_attribute ready_attr = __ATTR_RO(ready);
static struct attribute *base_attrs[] = { &ready_attr.attr, &state_attr.attr, &thermal_mask_attr.attr, &debug_attr.attr, NULL };
static const struct attribute_group base_group = { .attrs = base_attrs };

int __init x80_stock_qos_control_init(struct kobject *rsc_root);
int x80_stock_tick_control_init(struct kobject *rsc_root);
int x80_stock_qos_knobs_control_init(struct kobject *rsc_root);

int __init rsc_base_core_init(void)
{
 int ret;
 rsc_root_dir = kobject_create_and_add("rsc", NULL);
 if (!rsc_root_dir)
  return -ENOMEM;
 ret = sysfs_create_group(rsc_root_dir, &base_group);
 if (ret) {
  kobject_put(rsc_root_dir);
  rsc_root_dir = NULL;
  return ret;
 }
 ret = x80_stock_tick_control_init(rsc_root_dir);
 if (ret)
  return ret;
 ret = x80_stock_qos_knobs_control_init(rsc_root_dir);
 if (ret)
  return ret;
 ret = x80_stock_qos_control_init(rsc_root_dir);
 if (ret)
  return ret;
 raw_spin_lock_irq(&state_lock);
 armed = true;
 raw_spin_unlock_irq(&state_lock);
 return 0;
}
/* Called directly by cpufreq_core_init. Permanent built-in, no hook or exit. */
MODULE_LICENSE("GPL");
