// SPDX-License-Identifier: GPL-2.0
/*
 * Compatibility exports for the vivo vendor modules shipped with the X80.
 *
 * The Neo7 5.10.66 source does not contain several vivo-private helpers that
 * the X80 vendor ramdisk modules import.  Keep these fallbacks deliberately
 * conservative: disable the optional paths and let the callers use their
 * normal fallback implementations.
 */
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/x80_stock_qos_private.h>
#include <linux/types.h>
#include <linux/jiffies.h>
#include <linux/x80_stock_qos_private.h>
#include <linux/build_bug.h>
#include <linux/mmzone.h>

/* Verified against the stock X80 Image and shipped system_heap.ko. */
static_assert(sizeof(struct zone) == 0x880);
static_assert(offsetof(struct zone, name) == 0x80);
static_assert(offsetof(struct zone, free_area) == 0xc0);
static_assert(offsetof(struct zone, vm_stat) == 0x7c0);
static_assert(offsetof(struct pglist_data, node_zonelists) == 0x1980);
static_assert(MIGRATE_TYPES == 9);
static_assert(NR_VM_ZONE_STAT_ITEMS == 17);
static_assert(PB_migratetype_bits == 7);

/* Optional THP-pool allocation: NULL makes system_heap use its normal path. */
struct page *alloc_pages_from_thp_pool(gfp_t gfp_mask, unsigned int order,
					       int allow_charge,
					       struct list_head *free_list)
{
	return NULL;
}
EXPORT_SYMBOL(alloc_pages_from_thp_pool);

/* Pages not allocated by the absent pool are ordinary pages. */
bool is_thppool_page(struct page *page)
{
	return false;
}
EXPORT_SYMBOL(is_thppool_page);

static_assert(offsetof(struct task_struct, flags) == 0x44);
static_assert(offsetof(struct task_struct, exit_signal) == 0x56c);
static_assert(offsetof(struct task_struct, real_cred) == 0x778);
static_assert(offsetof(struct task_struct, comm) == 0x790);
static_assert(offsetof(struct cred, uid) == 4);
static_assert(PF_KTHREAD == 0x00200000);

bool is_task_crucial(struct task_struct *task);
bool is_task_crucial(struct task_struct *task)
{
	int qos = get_task_qos(task);
	kuid_t uid;

	/* Stock null/global-off get_task_qos returns0 before task dereference. */
	if (qos == 6)
		return true;
	if (qos < 3)
		return false;
	if (x80_stock_task_read_u8(task, X80_QOS_FLAGS_D6D) & 0x03)
		return true;
	if (READ_ONCE(task->flags) & PF_KTHREAD)
		return false;
	if (READ_ONCE(task->exit_signal) < 0)
		return false;

	/* task_uid's RCU read-side critical section matches stock real_cred. */
	uid = task_uid(task);
	if (__kuid_val(uid) > 10000U)
		return true;
	/* Exact six-byte prefix comparison, not NUL-inclusive exact comm. */
	return !strncmp(task->comm, "usap64", 6);
}
EXPORT_SYMBOL(is_task_crucial);

/* Stock getter semantics; real shared-list owner is separate. */
extern int rsc_qos_enable;

int get_task_qos(struct task_struct *task)
{
	u8 flags, policy, floor;
	unsigned int qos;
	u64 runtime_end, runtime, expires, now;

	/* +473de0..f0: null and global-off bypass policy AND both floor bytes. */
	if (!task || !READ_ONCE(rsc_qos_enable))
		return 0;

	/* +473df4..e04: acquire-load and immediate 6; floors do NOT apply. */
	if (x80_stock_task_boost_acquire(task))
		return 6;

	/* +473e08..e2c: inherited high flag combination gives base class5. */
	flags = x80_stock_task_read_u8(task, X80_QOS_FLAGS_D6D);
	if ((flags & 0x40) && (flags & 0x03)) {
		policy = x80_stock_task_read_u8(task, X80_QOS_FLAGS_D6F);
		if (policy & 0x25) {
			qos = 5;
			goto floors;
		}
	}

	/*
	 * +473e30/+473e90..eb0: active SVP wins when runtime remainder or
	 * jiffies remainder has a nonnegative 64-bit sign. Runtime is checked
	 * first; equality qualifies. Read jiffies only on the reachable path.
	 */
	if (x80_stock_task_read_u8(task, X80_QOS_SVP_ACTIVE)) {
		runtime_end = x80_stock_task_read_u64(task,
						    X80_QOS_SVP_RUNTIME_END);
		runtime = READ_ONCE(task->se.sum_exec_runtime);
		if (x80_stock_delta_nonnegative(runtime_end, runtime)) {
			qos = 4;
			goto floors;
		}
		expires = x80_stock_task_read_u64(task, X80_QOS_SVP_EXPIRES);
		now = (u64)READ_ONCE(jiffies);
		if (x80_stock_delta_nonnegative(expires, now)) {
			qos = 4;
			goto floors;
		}
	}

	/* +473e38..e68: exact priority order; class2 beats class1. */
	flags = x80_stock_task_read_u8(task, X80_QOS_FLAGS_E45);
	if (flags & 0x01) {
		qos = 4;
		goto floors;
	}
	policy = x80_stock_task_read_u8(task, X80_QOS_FLAGS_D6F);
	if ((flags & 0x20) || (policy & 0x25))
		qos = 3;
	else if (flags & 0x10)
		qos = 2;
	else
		qos = ((flags | policy) >> 6) & 1;

floors:
	/*
	 * +473e6c..e80: full unsigned byte floors, NOT clamped to class6.
	 * Stock first CSEL uses HI and second GT; all zero-extended values
	 * are 0..255 so both are ordinary max for this reachable range.
	 */
	floor = x80_stock_task_read_u8(task, X80_QOS_FLOOR_E46);
	if (qos <= floor)
		qos = floor;
	floor = x80_stock_task_read_u8(task, X80_QOS_FLOOR_E6B);
	if ((int)qos <= (int)floor)
		qos = floor;
	return qos;
}
EXPORT_SYMBOL(get_task_qos);

/* zcache state notification is optional; leave it as a no-op. */
void write_zcache_state(int enabled)
{
}
EXPORT_SYMBOL(write_zcache_state);

/* Stock active QoS policy: real fair/list/tick/BW/exit owners restored in #16.
 * Optional THP pool below remains a separate historical unimplemented feature.
 */
int rsc_qos_enable = 1;
EXPORT_SYMBOL(rsc_qos_enable);

int sysctl_allow_alloc_from_thppool;
EXPORT_SYMBOL(sysctl_allow_alloc_from_thppool);

/* Incremented with a 32-bit atomic (LDADDL in vendor system_heap.ko). */
atomic_t nr_dmapages_in_thppool = ATOMIC_INIT(0);
EXPORT_SYMBOL(nr_dmapages_in_thppool);

/* Real 32-bit pool accounting; no pages enter the disabled optional pool. */
atomic_t nr_ezrampages_in_thppool = ATOMIC_INIT(0);
EXPORT_SYMBOL(nr_ezrampages_in_thppool);

// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC CANDIDATE: real stock policy-byte setters and fork inheritance.
 * Reachable stage, not the complete shared SVP/QoS list owner.
 * No policy mask guessed, no global enable/disable shortcut, no padding.
 */
#include <linux/kthread.h>
#include <asm/cmpxchg.h>
#include <linux/x80_stock_qos_private.h>

/* Private names keep unrecovered external source prototypes unclaimed. */
void x80_stock_sys_set_qos_level(struct task_struct *task, u8 flags, u8 set);
void x80_stock_fork_set_rsc_qos_level(struct task_struct *child);

/*
 * CAS preserves neighbors and uses full-order cmpxchg just as stock CASALB
 * (or LDXRB/STLXRB+dmb fallback).  The stock setter has TWO CAS loops:
 * clear first, then OR; do not merge them and silently change observation.
 */
static void x80_stock_qos_byte_clear_then_or(u8 *address, u8 keep, u8 bits,
					  bool apply_or)
{
	u8 old, new;

	do {
		old = smp_load_acquire(address);
		new = old & keep;
	} while (cmpxchg(address, old, new) != old);
	if (!apply_or)
		return;
	do {
		old = smp_load_acquire(address);
		new = old | bits;
	} while (cmpxchg(address, old, new) != old);
}

static void x80_stock_qos_byte_or(u8 *address, u8 bits)
{
	u8 old, new;

	do {
		old = smp_load_acquire(address);
		new = old | bits;
	} while (cmpxchg(address, old, new) != old);
}

/* Stock Image+47813c..478260. No list admission or PID registry here. */
void x80_stock_sys_set_qos_level(struct task_struct *task, u8 flags, u8 set)
{
	u8 *policy;

	if (!task)
		return;
	policy = x80_stock_task_oem_mut(task) + X80_QOS_FLAGS_E45;
	/* +478154..1ac/+478230: keep bits1..3, then OR caller flags. */
	x80_stock_qos_byte_clear_then_or(policy, 0x0e, flags, set != 0);
}

/*
 * Stock Image+4744e4..4747b4, called only for an unpublished fork child
 * AFTER its exact postdup OEM reset. Uses current parent, not child parent
 * pointers. Parent byte7 means no inheritance. kthreadd supplies D6F bit5.
 * This must not be run before/reset after the generic postdup reset.
 */
void x80_stock_fork_set_rsc_qos_level(struct task_struct *child)
{
	struct task_struct *parent = current;
	u8 parent_policy, parent_system, bits;
	u8 *oem;

	if (!child)
		return;
	parent_policy = x80_stock_task_read_u8(parent, X80_QOS_FLAGS_D6F);
	if ((s8)parent_policy < 0)
		return;
	parent_system = x80_stock_task_read_u8(parent, X80_QOS_FLAGS_E45);
	if ((s8)parent_system < 0)
		return;
	oem = x80_stock_task_oem_mut(child);
	if (parent_policy & 0x40)
		x80_stock_qos_byte_or(oem + X80_QOS_FLAGS_D6F, 0x40);
	else if (parent_policy & 0x20)
		x80_stock_qos_byte_or(oem + X80_QOS_FLAGS_D6F, 0x20);
	if (READ_ONCE(kthreadd_task) == parent)
		x80_stock_qos_byte_or(oem + X80_QOS_FLAGS_D6F, 0x20);

	/* +47451c reloads current's E45 after the policy inheritance. */
	parent_system = x80_stock_task_read_u8(parent, X80_QOS_FLAGS_E45);
	if (parent_system & 0x01)
		bits = 0x01;
	else if (parent_system & 0x20)
		bits = 0x20;
	else if (parent_system & 0x40)
		bits = 0x40;
	else if (parent_system & 0x10)
		bits = 0x10;
	else
		return;
	x80_stock_qos_byte_or(oem + X80_QOS_FLAGS_E45, bits);
}

// SPDX-License-Identifier: GPL-2.0
/*
 * DIAGNOSTIC CANDIDATE: genuine storage for unchanged scheduler.ko producers.
 * Not completion of RSC scheduler policy. No fabricated counter increments.
 * Exact genksyms types verified against the unchanged module's __versions.
 * uint aliases or flattened group arrays are NOT ABI-equivalent declarations.
 *
 * Counter producers in official scheduler.ko .text:
 *  preempt[4] 5ea8..5eb8; [5]6080..608c; [6]60d4..60dc.
 *  launcher_suc605c..6068; launcher_fail60f4..610c.
 *  findcpu_mid9244..9250; newidle b7fc..b804.
 * These are real ordinary 32-bit loads/adds/stores, NOT atomic_t operations.
 * Additional stock core/diagnostic readers/writers remain to be integrated.
 */
#include <linux/build_bug.h>
#include <linux/export.h>
#include <linux/types.h>

u8 rsc_cpu_qos_enable = 1;
/* Stock __ksymtab_gpl; preserve ordinary vs GPL admission distinction. */
EXPORT_SYMBOL_GPL(rsc_cpu_qos_enable);

/* Index0=task flags bit22 clear; index1=set. Columns are group selectors. */
int rsc_cpulimit_group1[2][2] = { { -1, -1 }, { 3, 3 } };
EXPORT_SYMBOL(rsc_cpulimit_group1);
int rsc_cpulimit_group2[2][2] = { { -1, -1 }, { -1, -1 } };
EXPORT_SYMBOL(rsc_cpulimit_group2);

/* Genuine statistics owned here and updated by unchanged official module. */
u32 rsc_preempt_boost_rt_skip[12];
EXPORT_SYMBOL(rsc_preempt_boost_rt_skip);
u32 rsc_launcher_boost_select_suc_count;
EXPORT_SYMBOL(rsc_launcher_boost_select_suc_count);
u32 rsc_launcher_boost_select_fail_count;
EXPORT_SYMBOL(rsc_launcher_boost_select_fail_count);
u32 rsc_svp_newidle_balance_count;
EXPORT_SYMBOL(rsc_svp_newidle_balance_count);
u32 rsc_dynamic_svp_task_cfs_findcpu_boost_mid_count;
EXPORT_SYMBOL(rsc_dynamic_svp_task_cfs_findcpu_boost_mid_count);

static_assert(sizeof(rsc_cpu_qos_enable) == 1);
static_assert(sizeof(rsc_cpulimit_group1) == 16);
static_assert(sizeof(rsc_cpulimit_group2) == 16);
static_assert(sizeof(rsc_preempt_boost_rt_skip) == 48);
static_assert(sizeof(rsc_launcher_boost_select_suc_count) == 4);
static_assert(sizeof(rsc_launcher_boost_select_fail_count) == 4);
static_assert(sizeof(rsc_svp_newidle_balance_count) == 4);
static_assert(sizeof(rsc_dynamic_svp_task_cfs_findcpu_boost_mid_count) == 4);
