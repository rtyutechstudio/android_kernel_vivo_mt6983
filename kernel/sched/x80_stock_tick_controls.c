// SPDX-License-Identifier: GPL-2.0
/* Real stock sysfs paths/protocols, restored source candidate. */
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include <linux/x80_stock_qos_private.h>
#include <linux/vivo_x80_rsc_base.h>
#include <linux/sched/task.h>
#include "x80_stock_surface_policy.h"

unsigned int sysctl_rsc_switch = 0xf391;
u32 rsc_preempt_disable_writelock_max_time_jiffies = 26;
u32 rsc_preempt_disable_readlock_max_time_jiffies = 13;
u32 rsc_preempt_disable_run_max_time_ns = 8000000;
/* These genuine diagnostic statistics are BSS in stock, not synthetic
 * status flags. Their scheduler producers must use this single storage. */
u32 rsc_dopreempt_fail_count;
u32 rsc_preempt_count[8];
u32 rsc_preempt_disable_count[8];
u64 rsc_lockboost_preempt_disable_boost_count;
u32 rsc_lockboost_preempt_disable_boost_timeout_count;
extern u32 rsc_preempt_disable_overtime_count;
extern u32 rsc_preempt_disable_overjifftime_count;
extern u32 rsc_preempt_disable_overruntime_count;
extern u16 rsc_surfaceflinger_pid;
static DEFINE_MUTEX(rsc_switch_lock);
static const char *const rsc_switch_names[17] = {
 "BINDER_LIMIT_NORMAL", "BINDER_PROMOTE_NORMAL", "BINDER_LIMIT_AGGRESS",
 "BINDER_PROMOTE_AGGRESS", "BOOST_TASKKILL", "PRE_PAGEFAULT_PH",
 "BINDER_LIMIT_EXCEPT_FG_TASK", "APP_LAUNCH_BOOST", "APP_LAUNCH_BY_FRAMEWORK",
 "APP_LAUNCH_BINDER_BOOST", "APP_LAUNCH_NEW_SPF_DEADLOCK_PATCH_PH",
 "SCHED_SLEEPING_BLOCKED_REASON_ENABLE", "RSC_LOCK_BOOST_ENABLE",
 "RSC_BINDER_BOOST_ENABLE", "RSC_LOCK_BOOST_FUTEX_ENABLE",
 "RSC_LOCK_BOOST_TO_BIGCORE", "RSC_LOCK_NOT_BOOST_RENDER"
};
static ssize_t show_rsc_switch(struct kobject *kobj,
 struct kobj_attribute *attr, char *buf)
{
 ssize_t len;unsigned int i;
 len=scnprintf(buf,PAGE_SIZE,"0x%x\n",READ_ONCE(sysctl_rsc_switch));
 for(i=0;i<17;i++){
  if(!(READ_ONCE(sysctl_rsc_switch)&(1U<<i)))
   len+=scnprintf(buf+len,PAGE_SIZE-len,"NO_");
  len+=scnprintf(buf+len,PAGE_SIZE-len,"%s %lx\t",rsc_switch_names[i],1UL<<i);
 }
 len+=scnprintf(buf+len,PAGE_SIZE-len,"\nexample, set or clear bit1:\nsetval: echo        0x1 > /sys/rsc/rsc_switch\n clear: echo 0x80000001 > /sys/rsc/rsc_switch\n");
 return len;
}
static ssize_t store_rsc_switch(struct kobject *kobj,
 struct kobj_attribute *attr,const char *buf,size_t count)
{
 unsigned int value,old,new;int ret,surface=0;struct task_struct *task;
 ret=kstrtouint(buf,16,&value);
 if(ret){
  printk(KERN_ERR "^RSC [ERROR]set rsc_switch %x val: %x error! ret: %d buf: %s\n",READ_ONCE(sysctl_rsc_switch),0,ret,buf);
  return ret;
 }
 mutex_lock(&rsc_switch_lock);
 old=READ_ONCE(sysctl_rsc_switch);
 if(value&0x80000000U){
  new=old&~value;
  WRITE_ONCE(sysctl_rsc_switch,new);
 }else{
  new=old|value;
  if((5U&~new)!=0){
   /* Raw BICS wzr,#5,new branches to success when any required bit
    * absent. Only BOTH bits present reaches ERROR; preserve actual code. */
  }else{
   printk(KERN_ERR "^RSC [ERROR]set rsc_switch %x val: %x BINDER_LIMIT_NORMAL %lx and BINDER_LIMIT_AGGRESS %lx should be mutual exclusion.echo 0x80000005 > /sys/rsc/rsc_switch then echo 0x4 > /sys/rsc/rsc_switch\n",old,value,1UL,4UL);
   mutex_unlock(&rsc_switch_lock);return -EINVAL;
  }
  WRITE_ONCE(sysctl_rsc_switch,new);
  if((value&0x80004000U)==0x4000U){
   /* Corrective held identity: avoid truncated/reusable PID and namespaces. */
    task=x80_stock_surface_get();
   if(task){
    u64 *boost=&task->android_oem_data1[21];
    if(!(smp_load_acquire(boost)&0xf0000000ULL)){
     set_bit(28,(unsigned long *)boost);surface=1;
    }
   }
   if(task)put_task_struct(task);
  }
 }
 printk(KERN_WARNING "^RSC %s rsc_switch %x -> %x val: %x surface%d\n",
  value&0x80000000U?"clear":"set",old,READ_ONCE(sysctl_rsc_switch),value,surface);
 mutex_unlock(&rsc_switch_lock);return count;
}
static ssize_t fix_pri_inv_show(struct kobject *kobj,
 struct kobj_attribute *attr,char *buf)
{
 ssize_t len;unsigned int i,n;
 len=snprintf(buf,PAGE_SIZE,"maxboost: writelock %2u readlock %2u runtime %2u ms\n",
  (u32)(READ_ONCE(rsc_preempt_disable_writelock_max_time_jiffies)*1000U)/250U,
  (u32)(READ_ONCE(rsc_preempt_disable_readlock_max_time_jiffies)*1000U)/250U,
  READ_ONCE(rsc_preempt_disable_run_max_time_ns)/1000000U);
 len+=scnprintf(buf+len,PAGE_SIZE-len,"force preempt_disable: %5u (%5u + %5u) dopreemptfail %3u\n",
  READ_ONCE(rsc_preempt_disable_overtime_count),
  READ_ONCE(rsc_preempt_disable_overjifftime_count),
  READ_ONCE(rsc_preempt_disable_overruntime_count),READ_ONCE(rsc_dopreempt_fail_count));
 n=READ_ONCE(rsc_dopreempt_fail_count);
 if(n)for(i=0;i<min_t(unsigned int,n,8);i++)
  len+=scnprintf(buf+len,PAGE_SIZE-len,"%d: %x %x ",i,READ_ONCE(rsc_preempt_count[i]),READ_ONCE(rsc_preempt_disable_count[i]));
 len+=scnprintf(buf+len,PAGE_SIZE-len,"\n");
 len+=scnprintf(buf+len,PAGE_SIZE-len,"rsc preempt_disable boost count %8llu timeout %4u\n",
  READ_ONCE(rsc_lockboost_preempt_disable_boost_count),
  READ_ONCE(rsc_lockboost_preempt_disable_boost_timeout_count));
 return len;
}
static ssize_t fix_pri_inv_store(struct kobject *kobj,
 struct kobj_attribute *attr,const char *buf,size_t count)
{
 unsigned int a=0,b=0,c=0;int n=sscanf(buf,"%u %u %u",&a,&b,&c);
 if(n<1||n>3)return -EINVAL;
 WRITE_ONCE(rsc_preempt_disable_writelock_max_time_jiffies,(u32)(a*250U)/1000U);
 if(n>=2)WRITE_ONCE(rsc_preempt_disable_readlock_max_time_jiffies,(u32)(b*250U)/1000U);
 if(n==3)WRITE_ONCE(rsc_preempt_disable_run_max_time_ns,c*1000000U);
 return count;
}
static struct kobj_attribute rsc_switch_attr=__ATTR(rsc_switch,0660,show_rsc_switch,store_rsc_switch);
static struct kobj_attribute kobj_attr_fix_pri_inv=__ATTR(fix_pri_inv,0660,fix_pri_inv_show,fix_pri_inv_store);
static struct attribute *x80_tick_control_attrs[]={&rsc_switch_attr.attr,&kobj_attr_fix_pri_inv.attr,NULL};
static const struct attribute_group x80_tick_control_group={.attrs=x80_tick_control_attrs};
int x80_stock_tick_control_init(struct kobject *root);
int x80_stock_tick_control_init(struct kobject *root)
{
 int ret=sysfs_create_group(root,&x80_tick_control_group);
 if(ret)return ret;
 ret=rsc_chown_to_system(root,&rsc_switch_attr.attr);
 if(!ret)ret=rsc_chown_to_system(root,&kobj_attr_fix_pri_inv.attr);
 if(ret)sysfs_remove_group(root,&x80_tick_control_group);
 return ret;
}
