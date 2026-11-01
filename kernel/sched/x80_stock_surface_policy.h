/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_SURFACE_POLICY_H
#define _X80_STOCK_SURFACE_POLICY_H
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/x80_stock_qos_private.h>
extern u16 rsc_surfaceflinger_pid;
extern struct task_struct *rsc_surfaceflinger;
void x80_stock_surface_publish(struct task_struct *task);
void x80_stock_surface_exit(struct task_struct *task);
/* Caller owns returned reference; never re-resolve a truncated u16 PID. */
struct task_struct *x80_stock_surface_get(void);
static_assert(offsetof(struct task_struct, exit_signal) == 0x56c);
/* Genuine producer from __sched_setscheduler success tail, after preempt_enable.
 * New requested RT policy, process leader (exit_signal>=0), uid1000, exact
 * TASK_COMM_LEN name match, no existing nibble7. No hardcoded process id.
 * Deliberate lifetime correction: publication owns a ref and exit clears it
 * under a private spinlock; caller must wire the explicit exit hook. */
static inline void x80_stock_surface_policy_changed(struct task_struct *task,
		int new_policy)
{
	u64 *boost;
	unsigned int uid;
	if ((unsigned int)(new_policy - 1) >= 2 ||
	    READ_ONCE(task->exit_signal) < 0)
		return;
	rcu_read_lock();
	uid = __kuid_val(__task_cred(task)->uid);
	rcu_read_unlock();
	if (uid != 1000 || strncmp("surfaceflinger", task->comm, TASK_COMM_LEN))
		return;
	boost = &task->android_oem_data1[21];
	if (smp_load_acquire(boost) & 0xf0000000ULL)
		return;
	set_bit(28, (unsigned long *)boost);
	x80_stock_surface_publish(task);
	rcu_read_lock();
	uid = __kuid_val(__task_cred(task)->uid);
	rcu_read_unlock();
	printk(KERN_WARNING "^RSC appboost setting surfaceflinger svpforce in setscheduler uid %5d %16s %5d dyn: %8llx <- uid %5d %16s %5d\n",
		uid, task->comm, task->pid, smp_load_acquire(boost),
		__kuid_val(current->cred->uid), current->comm, current->pid);
}
#endif
