// SPDX-License-Identifier: GPL-2.0
#include "sched.h"
#include "x80_stock_qos_trace_owner.h"
#include <trace/events/sched.h>
void x80_stock_trace_svp_queue_op(struct task_struct *task, const char *message)
{
 trace_sched_svp_queue_op(task, message);
}
