// SPDX-License-Identifier: GPL-2.0
/* Recovered identity globals plus explicit lifetime correction requested by
 * integration parent: own one reference while published, clear on exit. */
#include <linux/sched/task.h>
#include <linux/spinlock.h>
u16 rsc_surfaceflinger_pid;
struct task_struct *rsc_surfaceflinger;
static DEFINE_SPINLOCK(x80_surface_identity_lock);
void x80_stock_surface_publish(struct task_struct *task);
void x80_stock_surface_exit(struct task_struct *task);
struct task_struct *x80_stock_surface_get(void);
struct task_struct *x80_stock_surface_get(void)
{
 struct task_struct *task;
 unsigned long flags;
 spin_lock_irqsave(&x80_surface_identity_lock, flags);
 task = rsc_surfaceflinger;
 if (task && !(READ_ONCE(task->flags) & PF_EXITING))
  get_task_struct(task);
 else
  task = NULL;
 spin_unlock_irqrestore(&x80_surface_identity_lock, flags);
 return task;
}
void x80_stock_surface_publish(struct task_struct *task)
{
 struct task_struct *old=NULL;unsigned long flags;
 spin_lock_irqsave(&x80_surface_identity_lock,flags);
 /* Exit hook after PF_EXITING and this lock close publication-vs-exit race. */
 if(!(READ_ONCE(task->flags)&PF_EXITING) && rsc_surfaceflinger!=task){
  get_task_struct(task);old=rsc_surfaceflinger;
  WRITE_ONCE(rsc_surfaceflinger,task);
  WRITE_ONCE(rsc_surfaceflinger_pid,(u16)READ_ONCE(task->pid));
 }
 spin_unlock_irqrestore(&x80_surface_identity_lock,flags);
 if(old)put_task_struct(old);
}
void x80_stock_surface_exit(struct task_struct *task)
{
 struct task_struct *old=NULL;unsigned long flags;
 spin_lock_irqsave(&x80_surface_identity_lock,flags);
 if(rsc_surfaceflinger==task){
  old=rsc_surfaceflinger;
  WRITE_ONCE(rsc_surfaceflinger_pid,0);
  WRITE_ONCE(rsc_surfaceflinger,NULL);
 }
 spin_unlock_irqrestore(&x80_surface_identity_lock,flags);
 if(old)put_task_struct(old);
}
