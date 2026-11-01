// SPDX-License-Identifier: GPL-2.0
/* Candidate scheduler translation unit. Not added to production Makefile.
 * Installed location kernel/sched; __has_include fallback is diagnostic-only.
 * No EXPORT_SYMBOL additions: none of these stock functions is exported.
 * Five enqueue argument register widths proved by stock caller assembly;
 * original vendor declaration spelling unavailable, not a CRC claim. */
#include "sched.h"
#include "x80_stock_dynamic_svp_owner.h"
#include "x80_stock_dynamic_svp_rcu_owner.h"

int dynamic_svp_enqueue_internal(struct task_struct *p,
	struct task_struct *requester, int reason, int depth, const char *dependency);
void __dynamic_svp_dequeue(struct task_struct *p, int reason);
void dynamic_svp_dequeue(struct task_struct *p, int reason);
int x80_stock_boost_rcu_blockchain_entry(int raw_pid);

int dynamic_svp_enqueue_internal(struct task_struct *p,
	struct task_struct *requester, int reason, int depth, const char *dependency)
{
	return x80_stock_dynamic_svp_enqueue_internal(p, requester, reason,
		depth, dependency);
}
void __dynamic_svp_dequeue(struct task_struct *p, int reason)
{
	x80_stock___dynamic_svp_dequeue(p, reason);
}
void dynamic_svp_dequeue(struct task_struct *p, int reason)
{
	__dynamic_svp_dequeue(p, reason);
}
/* Called only from recovered RCU-read-locked blockchain callsites; this does
 * NOT add a new user-visible trigger or substitute a disconnected timer. */
int x80_stock_boost_rcu_blockchain_entry(int raw_pid)
{
	return x80_stock_rsc_boost_rcu_blockchain(raw_pid);
}
