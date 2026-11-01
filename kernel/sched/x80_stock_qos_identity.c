// SPDX-License-Identifier: GPL-2.0
/* Finite PR_SET_NAME CTS trace suppression producer, stock 18a544..18a574,
 * 18b2c0..18b2e0. No fake zero external is used to bypass tracing.
 */
#include <linux/build_bug.h>
#include <linux/cred.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>
#include "x80_stock_qos_identity.h"

int in_cts_atracetest;
static_assert(offsetof(struct task_struct, exit_signal) == 0x56c);
static_assert(offsetof(struct task_struct, real_cred) == 0x778);
static_assert(offsetof(struct task_struct, cred) == 0x780);
static_assert(sizeof(((struct task_struct *)0)->comm) == 16);

void x80_stock_qos_prctl_name(struct task_struct *task, const unsigned char *comm)
{
	const struct cred *cred;
	unsigned int uid;

	/* Called for current, after successful copy and BEFORE set_task_comm.
	 * Stock uses task->cred->uid (not euid, not namespace mapped UID).
	 */
	cred = rcu_dereference_protected(task->cred, task == current);
	uid = __kuid_val(cred->uid);
	if (uid < 10000 || READ_ONCE(task->exit_signal) < 0)
		return;
	if (memcmp(comm, "s.atracetestapp", 15))
		return;
	WRITE_ONCE(in_cts_atracetest, 1);
	printk("start ard cts atracetest %5d %16s %5d\n", uid, comm, task->pid);
	/* Stock has no clearing writer or exit reference: deliberately sticky.
	 * No task pointer retained, so no task ref/RCU lifetime obligation.
	 */
}
