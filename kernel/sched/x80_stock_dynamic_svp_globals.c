// SPDX-License-Identifier: GPL-2.0
/* Candidate owner definitions. Every counter has recovered mutator writer. */
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/timer.h>
#include <linux/sched.h>
unsigned long dynamic_svp_count[7]; /* +46f88c..46f8a0 */
unsigned int dynamic_svp_over_count[7]; /* +46f988..46f990 */
int dynamic_svp_max_depth; /* +46f9d4..46f9e4 */
unsigned int dynamic_svp_depth_count[16]; /* +46fa08..46fa14; depth>14 slot15 */
unsigned int svp_max_dynamic_granularity_ns = 32000000;
unsigned int svp_max_dynamic_granularity_ns_for_binder = 64000000;
/* Stock +2bd7174. svp_stat_store field2 writes this ms counterpart. */
int svp_max_dynamic_granularity = 32;
/* +1954a0..1956d0 actual timer producer and +47099c consumer. */
int rsc_svp_rcu_blockchain_boost_task_count;
atomic_t rsc_svp_rcu_blockchain_boost = ATOMIC_INIT(0);
struct task_struct *rsc_svp_rcu_blockchain_task[30];
struct timer_list rsc_svp_rcu_blockchain_timer;
unsigned int rsc_sync_rcu_blockchain_boost_time;
unsigned int rsc_sync_rcu_blockchain_kwork_run_unrun_count[2];
unsigned int rsc_svp_rcu_blockchain_total_boost_task_count;
unsigned int rsc_svp_rcu_blockchain_boost_task_over_count;
unsigned int rsc_svp_rcu_blockchain_boost_restore_skip_count;
