/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_IDENTITY_H
#define _X80_STOCK_QOS_IDENTITY_H
#include <linux/sched.h>
/* Exact sticky stock trace suppressor, not a QoS enable/readiness flag. */
extern int in_cts_atracetest;
void x80_stock_qos_prctl_name(struct task_struct *task, const unsigned char *comm);
#endif
