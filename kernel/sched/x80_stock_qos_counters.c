// SPDX-License-Identifier: GPL-2.0
/* Original zero-initialized BSS, not fabricated readiness flags.
 * dynamic_svp_timeout_deq_count: [2e1bb38,2e1bb54), ordinary seven u32s.
 * rsc_qos_enqueue_count: [2e3c228,2e3c258), six atomic64 counters.
 * rsc_qos_enqueue_total_time: [2e3c258,2e3c288), six atomic64 counters.
 * qos_stat_store explicitly resets each six-element array (476fc4..477000).
 * No timer/workqueue is implied: elapsed time is sampled from sched_clock
 * and e18 by qos_running_stat_record. Counter policy executes on dequeue.
 */
#include <linux/atomic.h>
#include <linux/types.h>
u32 dynamic_svp_timeout_deq_count[7];
atomic64_t rsc_qos_enqueue_count[6];
atomic64_t rsc_qos_enqueue_total_time[6];
