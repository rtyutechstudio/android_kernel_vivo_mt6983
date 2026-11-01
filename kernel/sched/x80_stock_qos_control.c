// SPDX-License-Identifier: GPL-2.0
/* Source-owned stock QoS policy control. Not an opaque-module interception.
 * Recovered Image 475430,475f6c,4760d0,477590,4779b4,478270,478940.
 * Integration candidate: call init only after all real scheduler owners exist.
 */
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pid_namespace.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/vivo_x80_rsc_base.h>
#include <linux/x80_stock_qos_private.h>
#include "x80_stock_qos_control.h"

extern int rsc_qos_enable;
extern u8 rsc_cpu_qos_enable;
static DEFINE_RAW_SPINLOCK(set_task_qos_lock);
static DEFINE_MUTEX(qos_cpu_enable_lock);
struct qos_pid_record {
 struct hlist_node hash;
 struct list_head list;
 u32 stamp_ms, owner_uid;
 u8 flags, set, all_tasks;
 /* Corrective native lifetime owner; never identify a task by reused number. */
 struct pid *identity;
};
static struct qos_pid_record records[256];
static struct hlist_head buckets[2048];
static LIST_HEAD(free_records);
static LIST_HEAD(active_records);
static unsigned int total;
static struct kobject *qos_root;

/* Stock multiplier is -GOLDEN_RATIO_32, not hash_32's generic fallback. */
static unsigned int qos_hash(pid_t pid)
{
 return ((u32)pid * 0x61c88647U) >> 21;
}
static struct qos_pid_record *qos_find(struct pid *identity)
{
 struct qos_pid_record *r;
 hlist_for_each_entry(r, &buckets[qos_hash(pid_nr(identity))], hash)
  if (r->identity == identity)
   return r;
 return NULL;
}
static void qos_delete(struct pid *identity)
{
 struct qos_pid_record *r = qos_find(identity);
 if (!r)
  return;
 hlist_del_init(&r->hash);
 list_move_tail(&r->list, &free_records);
 put_pid(r->identity);
 r->identity = NULL;
 --total;
}
static void qos_register(struct pid *identity, u8 flags, u8 set, u8 all_tasks)
{
 struct qos_pid_record *r = qos_find(identity);
 if (!r) {
  /* Stock registry failure is diagnostic-only: policy write still succeeds. */
  if (total == ARRAY_SIZE(records) || list_empty(&free_records)) {
   pr_warn_ratelimited("rsc qos PID registry full: %d\n", pid_nr(identity));
   return;
  }
  r = list_first_entry(&free_records, struct qos_pid_record, list);
  list_move_tail(&r->list, &active_records);
  r->identity = get_pid(identity);
  hlist_add_head(&r->hash, &buckets[qos_hash(pid_nr(identity))]);
  ++total;
 }
 r->flags = flags;
 r->set = set;
 r->all_tasks = all_tasks;
 r->stamp_ms = (u32)jiffies_to_msecs(jiffies);
 r->owner_uid = __kuid_val(current_uid());
}

static ssize_t set_task_qos_store(struct kobject *kobj,
 struct kobj_attribute *attr, const char *buf, size_t count)
{
 struct task_struct *task, *thread;
 struct pid *identity;
 pid_t pid = 0;
 u8 flags = 0, set = 0, all_tasks = 0;

 /* These are bit flags + set selector, NOT a QoS class0..6 interface. */
 if (sscanf(buf, "%d %hhu %hhu %hhu", &pid, &flags, &set, &all_tasks) != 4)
  return -EINVAL;
 raw_spin_lock(&set_task_qos_lock);
 rcu_read_lock();
 task = pid ? find_task_by_vpid(pid) : current;
 if (!task || !flags || (READ_ONCE(task->flags) & PF_EXITING) ||
     READ_ONCE(task->exit_state)) {
  rcu_read_unlock();
  raw_spin_unlock(&set_task_qos_lock);
  return -EINVAL;
 }
 identity = get_task_pid(task, PIDTYPE_PID);
 if (!identity) {
  rcu_read_unlock();
  raw_spin_unlock(&set_task_qos_lock);
  return -ESRCH;
 }
 get_task_struct(task);
 /* Hold RCU across thread traversal; registry holds its own PID identity ref.
  * Post-exit_signals cleanup closes the early-profile-notifier race.
  */
 if (all_tasks) {
  for_each_thread(task, thread) {
   get_task_struct(thread);
   if (!(READ_ONCE(thread->flags) & PF_EXITING) &&
       !READ_ONCE(thread->exit_state))
    x80_stock_sys_set_qos_level(thread, flags, set);
   put_task_struct(thread);
  }
 } else {
  x80_stock_sys_set_qos_level(task, flags, set);
 }
 put_task_struct(task);
 rcu_read_unlock();
 if (set)
  qos_register(identity, flags, set, all_tasks);
 else
  qos_delete(identity);
 put_pid(identity);
 raw_spin_unlock(&set_task_qos_lock);
 return count;
}

/* Avoid adding task_lock nesting beneath the stock raw registry lock.
 * A concurrent rename may produce a mixed diagnostic name, never an overread.
 */
static void qos_comm_snapshot(char dst[TASK_COMM_LEN], struct task_struct *task)
{
 unsigned int i;
 for (i = 0; i < TASK_COMM_LEN - 1; ++i)
  dst[i] = READ_ONCE(task->comm[i]);
 dst[TASK_COMM_LEN - 1] = '\0';
}
static ssize_t set_task_qos_show(struct kobject *kobj,
 struct kobj_attribute *attr, char *buf)
{
 struct qos_pid_record *r;
 struct task_struct *task, *leader;
 char comm[TASK_COMM_LEN], leader_comm[TASK_COMM_LEN];
 unsigned int nr = 0, now = (u32)jiffies_to_msecs(jiffies);
 int qos, leader_pid;
 ssize_t n;

 raw_spin_lock(&set_task_qos_lock);
 n = scnprintf(buf, PAGE_SIZE, "total: %u\n", total);
 n += scnprintf(buf + n, PAGE_SIZE - n,
  "NO \t  task.comm       task.pid \t  gl.comm \t   gl.pid \t qos(task) \t enable \t qos \t all_tasks \t elaps owner_uid\n");
 list_for_each_entry(r, &active_records, list) {
  if (n >= PAGE_SIZE - 1)
   break;
  strscpy(comm, "NULL", sizeof(comm));
  strscpy(leader_comm, "NULL", sizeof(leader_comm));
  leader_pid = 99999;
  qos = 0;
  rcu_read_lock();
  task = pid_task(r->identity, PIDTYPE_PID);
  if (task) {
   qos_comm_snapshot(comm, task);
   leader = READ_ONCE(task->group_leader);
   if (leader) {
    qos_comm_snapshot(leader_comm, leader);
    leader_pid = task_pid_nr_ns(leader, task_active_pid_ns(current));
   }
   qos = get_task_qos(task);
  }
  rcu_read_unlock();
  n += scnprintf(buf + n, PAGE_SIZE - n,
   "%2u\t%16s %6d\t\t%16s%8d\t%6d\t%10u\t\t%6u\t%12u\t%8u %9u\n",
   nr++, comm, pid_nr_ns(r->identity, task_active_pid_ns(current)),
   leader_comm, leader_pid, qos,
   r->set, r->flags, r->all_tasks, now - r->stamp_ms, r->owner_uid);
 }
 raw_spin_unlock(&set_task_qos_lock);
 return n;
}

/* Native corrective lifecycle owner. Called both by original early profile
 * subscriber and directly AFTER exit_signals sets PF_EXITING in do_exit.
 * If a store admitted before PF_EXITING, this final cleanup waits for its raw
 * lock and removes the entry. Stores after PF_EXITING cannot admit an entry.
 */
static void qos_registry_task_exit(struct task_struct *task)
{
 if (task) {
  raw_spin_lock(&set_task_qos_lock);
  qos_delete(task_pid(task));
  raw_spin_unlock(&set_task_qos_lock);
 }
}
/* Same exact native signature as x80_stock_surface_policy.h; no private
 * scheduler layout header needed for this lifetime call. Separate locks. */
extern void x80_stock_surface_exit(struct task_struct *task);
void x80_stock_qos_task_exit_finished(struct task_struct *task)
{
 x80_stock_surface_exit(task);
 qos_registry_task_exit(task);
}
static int qos_task_exit(struct notifier_block *nb, unsigned long event, void *data)
{
 qos_registry_task_exit(data);
 return NOTIFY_OK;
}
static struct notifier_block qos_exit_nb = { .notifier_call = qos_task_exit };

static ssize_t qos_enable_show(struct kobject *kobj,
 struct kobj_attribute *attr, char *buf)
{
 int enable;
 u8 cpu_enable;
 mutex_lock(&qos_cpu_enable_lock);
 enable = READ_ONCE(rsc_qos_enable);
 cpu_enable = READ_ONCE(rsc_cpu_qos_enable);
 mutex_unlock(&qos_cpu_enable_lock);
 return scnprintf(buf, PAGE_SIZE,
  "rsc_qos_enable: 0x%x, rsc_cpu_qos_enable: 0x%x\n", enable, cpu_enable);
}
static ssize_t qos_enable_store(struct kobject *kobj,
 struct kobj_attribute *attr, const char *buf, size_t count)
{
 unsigned int value;
 if (sscanf(buf, "%x", &value) != 1 || value > 1)
  return -EINVAL;
 mutex_lock(&qos_cpu_enable_lock);
 WRITE_ONCE(rsc_qos_enable, (int)value);
 WRITE_ONCE(rsc_cpu_qos_enable, (u8)value);
 mutex_unlock(&qos_cpu_enable_lock);
 return count;
}
static struct kobj_attribute set_task_qos_attr =
 __ATTR(set_task_qos, 0660, set_task_qos_show, set_task_qos_store);
static struct kobj_attribute qos_enable_attr =
 __ATTR(qos_enable, 0660, qos_enable_show, qos_enable_store);
static struct attribute *qos_attrs[] = {
 &set_task_qos_attr.attr, &qos_enable_attr.attr, NULL
};
static const struct attribute_group qos_group = { .attrs = qos_attrs };

/* Permanent built-in owner, called ONCE from real RSC source init after the
 * genuine enqueue/dequeue/timer/fair/tick/fork/getter owners are wired.
 * Caller must not publish an active gate if any of those integration gates fail.
 * profile_event_register refuses CONFIG_PROFILING=n rather than silently leak.
 */
int __init x80_stock_qos_control_init(struct kobject *rsc_root)
{
 unsigned int i;
 int ret;
 if (!rsc_root)
  return -ENODEV;
 for (i = 0; i < ARRAY_SIZE(records); ++i) {
  INIT_HLIST_NODE(&records[i].hash);
  INIT_LIST_HEAD(&records[i].list);
  list_add_tail(&records[i].list, &free_records);
 }
 ret = profile_event_register(PROFILE_TASK_EXIT, &qos_exit_nb);
 if (ret)
  return ret;
 qos_root = kobject_create_and_add("qos", rsc_root);
 if (!qos_root) {
  ret = -ENOMEM;
  goto unregister;
 }
 ret = sysfs_create_group(qos_root, &qos_group);
 if (ret)
  goto put;
 for (i = 0; qos_attrs[i]; ++i) {
  ret = rsc_base_chown_attr(qos_root, qos_attrs[i]);
  if (ret)
   goto remove;
 }
 return 0;
remove:
 sysfs_remove_group(qos_root, &qos_group);
put:
 kobject_put(qos_root);
 qos_root = NULL;
unregister:
 profile_event_unregister(PROFILE_TASK_EXIT, &qos_exit_nb);
 return ret;
}
