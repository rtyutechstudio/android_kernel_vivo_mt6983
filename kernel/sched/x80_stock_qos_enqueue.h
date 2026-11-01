/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_ENQUEUE_H
#define _X80_STOCK_QOS_ENQUEUE_H
#include <linux/types.h>
struct rq;
struct task_struct;
/* Caller holds genuine rq lock, owns task lifetime. */
void x80_stock_enqueue_svp_task(struct rq *rq, struct task_struct *task);
/* These wrappers lock stable task rq + pi_lock; callers must NOT hold either. */
int x80_stock_dynamic_qos_enqueue(struct task_struct *task, int qos);
void x80_stock_dynamic_qos_dequeue(struct task_struct *task);

extern int dynamic_svp_enqueue_enable;
extern u32 rsc_qos_debug;
extern u8 rsc_qos_bw_enable;
extern u32 rsc_max_qos_granularity_ns[6];
extern u64 rsc_max_qos_runtime_granularity_ns[6];
extern u64 rsc_max_qos_enqueue_granularity_ns[6];
extern u32 rsc_min_qos_migration_delay[6];
extern u32 rsc_qos_bw_quota;
extern u32 rsc_qos_bw_period;
extern u32 rsc_qos_reenqueue_time;
extern int svp_min_sched_delay_granularity;
extern u32 rsc_svp_preempt_pickup_count;
/* Existing #15 definitions; do not duplicate or replace their CRC types. */
extern int rsc_qos_enable;
extern u8 rsc_cpu_qos_enable;

/* Genuine trace implementation supplied by the matching dequeue/trace owner. */
void x80_stock_trace_svp_queue_op(struct task_struct *task, const char *op);
#endif
