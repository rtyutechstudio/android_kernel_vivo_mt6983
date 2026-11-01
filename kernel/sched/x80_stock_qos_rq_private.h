/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_RQ_PRIVATE_H
#define _X80_STOCK_QOS_RQ_PRIVATE_H
/*
 * Include only AFTER the scheduler's genuine private sched.h declares rq.
 * DIAGNOSTIC CANDIDATE. No replacement rq type or extra padding.
 */
#include <linux/x80_stock_qos_private.h>

static_assert(offsetof(struct rq, android_oem_data1) == 0x11c8);
static_assert(sizeof(((struct rq *)0)->android_oem_data1) == 0x80);
static_assert(offsetof(struct rq, android_oem_data1[1]) == 0x11d0);
static_assert(offsetof(struct rq, android_oem_data1[2]) == 0x11d8);
static_assert(offsetof(struct rq, android_oem_data1[8]) == 0x1208);
static_assert(offsetof(struct rq, cfs_tasks) == 0xc50);
static_assert(offsetof(struct rq, cpu_capacity) == 0xbf0);
static_assert(offsetof(struct rq, cpu_capacity_orig) == 0xbf8);
static_assert(sizeof(struct rq) == 0x1280);

static inline struct list_head *x80_stock_rq_shared_head(struct rq *rq)
{
	return (struct list_head *)&rq->android_oem_data1[1];
}

static inline u16 *x80_stock_rq_shared_count(struct rq *rq)
{
	/* +11ca proven halfword. Neighbor +11c8 low halfword is not renamed. */
	return (u16 *)((u8 *)rq->android_oem_data1 + 2);
}

/*
 * Stock svp_init_rq_data+4739f4 and sched_init+27c5df8..27c5e08.
 * Boot/per-CPU construction only, before publishing rq to callbacks.
 * This does NOT initialize the separate bandwidth/timer/aggregate owner.
 * Never call this to discard an active list; unlink+put policy is separate.
 */
static inline void x80_stock_qos_init_rq_shared_list(struct rq *rq)
{
	if (!rq)
		return;
	*(u32 *)&rq->android_oem_data1[0] = 0;
	INIT_LIST_HEAD(x80_stock_rq_shared_head(rq));
}
#endif
