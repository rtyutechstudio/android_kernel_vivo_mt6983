// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/export.h>
/* Genuine stock BSS, not asserted balanced: see counter evidence audit. */
atomic_t rsc_svp_task_cnt = ATOMIC_INIT(0);
DEFINE_PER_CPU(struct task_struct *, svp_balance_task);
/* Stock defaults +2bd7170, +2b8d58c, +2b8d588 respectively. */
int svp_min_migration_delay = 10;
/* Genuine exported stock data, Image +2b8d58c/+2b8d588, both initial -1.
 * Image readers include balance and dependency traversal. No writer has been
 * proved in the inspected Image/237 modules: do not invent a PID producer or
 * pretend these are cpulimit sysfs outputs. Preserve their real initial state.
 */
int rsc_first_candidate_pid = -1;
int rsc_second_candidate_pid = -1;
EXPORT_SYMBOL(rsc_first_candidate_pid);
EXPORT_SYMBOL(rsc_second_candidate_pid);
unsigned int rsc_heavy_task_push_cont_count;
unsigned int rsc_heavy_task_push_skip_count;
unsigned int rsc_heavy_task_push_suc_count;
