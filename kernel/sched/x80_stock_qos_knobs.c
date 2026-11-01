// SPDX-License-Identifier: GPL-2.0
/* Live policy backing owned by the rebuilt scheduler, recovered stock data. */
#include <linux/build_bug.h>
#include <linux/types.h>
#include "x80_stock_qos_enqueue.h"

/* Image 2ada650/658: real W owners. 65c is a byte despite symbol gap4. */
int dynamic_svp_enqueue_enable = 1;
u32 rsc_qos_debug = 2;
u8 rsc_qos_bw_enable = 1;

/* Image2ada660/690: real u64 runtime/enqueue thresholds, all six classes. */
u64 rsc_max_qos_runtime_granularity_ns[6] = {
	25000000ULL, 25000000ULL, 50000000ULL,
	50000000ULL, 50000000ULL, 50000000ULL,
};
u64 rsc_max_qos_enqueue_granularity_ns[6] = {
	50000000ULL, 50000000ULL, 50000000ULL,
	50000000ULL, 512000000ULL, 50000000ULL,
};
/* Image2ada6c0/6c4, read through W registers by bandwidth owner. */
u32 rsc_qos_bw_quota = 400;
u32 rsc_qos_bw_period = 450;
/* Image2ada6c8/6e0: six genuine u32 deadline and migration thresholds. */
u32 rsc_max_qos_granularity_ns[6] = {
	24000000U, 64000000U, 128000000U,
	128000000U, 256000000U, 36000000U,
};
u32 rsc_min_qos_migration_delay[6] = {
	64000000U, 32000000U, 16000000U,
	16000000U, 8000000U, 4000000U,
};
/* Genuine stock knobs/counter; zero defaults are not disabled stub owners. */
u32 rsc_qos_reenqueue_time;                 /* D 2ada5bc */
int svp_min_sched_delay_granularity;        /* B 2e1bb08, kstrtoint */
u32 rsc_svp_preempt_pickup_count;           /* W INC at1ee580..598 */
static_assert(sizeof(rsc_max_qos_runtime_granularity_ns) == 48);
static_assert(sizeof(rsc_max_qos_enqueue_granularity_ns) == 48);
static_assert(sizeof(rsc_max_qos_granularity_ns) == 24);
static_assert(sizeof(rsc_min_qos_migration_delay) == 24);
/*
 * No new export is invented: these are private built-in owner variables.
 * Already exported #15 rsc_qos_enable / rsc_cpu_qos_enable live elsewhere.
 * Retain their existing original .ko genksyms CRCs, typedefs and export class.
 */
