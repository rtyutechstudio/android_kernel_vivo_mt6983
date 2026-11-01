/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_ENQUEUE_OWNER_H
#define _X80_STOCK_QOS_ENQUEUE_OWNER_H
/* Shared typed accessors, not a second implementation of the enqueue owner. */
#include "x80_stock_qos_rq_private.h"
#include "x80_stock_qos_enqueue.h"
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <asm/cmpxchg.h>
enum x80_stock_qos_owner_offsets {
 X80_SVP_RQ_CLOCK = 0xa0,
 X80_QOS_DEADLINE = 0x100,
 X80_QOS_ENQUEUE_CLOCK = 0x108,
 X80_QOS_ENQUEUE_RUNTIME = 0x110,
 X80_QOS_ACCOUNT_RESET = 0x138,
};
static_assert(offsetof(struct task_struct, on_rq) == 0x80);
static_assert(offsetof(struct task_struct, se.avg.util_avg) == 0x270);
static_assert(sizeof(((struct task_struct *)0)->se.avg.util_avg) == sizeof(u64));
static inline void x80_stock_enqueue_store_u64(struct task_struct *p,
 unsigned int off, u64 value)
{
 WRITE_ONCE(*(u64 *)(x80_stock_task_oem_mut(p) + off), value);
}
static inline u64 *x80_stock_rq_qos_util_slot(struct rq *rq, unsigned int slot)
{
 return &rq->android_oem_data1[8 + slot];
}
#endif
