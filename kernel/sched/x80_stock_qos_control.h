/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_CONTROL_H
#define _X80_STOCK_QOS_CONTROL_H
#include <linux/init.h>
struct kobject;
struct task_struct;
/* Native final registry cleanup: MUST run after exit_signals/PF_EXITING. */
void x80_stock_qos_task_exit_finished(struct task_struct *task);
/* Source-only init after genuine scheduler/fork/timer ownership is complete.
 * Adds real /sys/rsc/qos/{set_task_qos,qos_enable}; no fabricated readiness.
 */
int __init x80_stock_qos_control_init(struct kobject *rsc_root);
#endif
