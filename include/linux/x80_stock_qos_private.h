/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X80_STOCK_QOS_PRIVATE_H
#define _X80_STOCK_QOS_PRIVATE_H
/*
 * DIAGNOSTIC SOURCE CANDIDATE, not a complete QoS owner.
 * Private, width-specific access into existing task OEM storage.  No inserted
 * task/rq fields, padding structs, fake exported types, or changed prototypes.
 * Offsets below are stock Image observations, not recovered member names.
 */
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/barrier.h>

enum x80_stock_task_oem_offset {
	X80_TASK_OEM_BASE = 0xd10,
	X80_TASK_OEM_SIZE = 0x200,
	X80_QOS_POLICY_RESET = 0x18,       /* absolute d28 */
	X80_QOS_POLICY_RESET_SIZE = 0x148, /* absolute [d28,e70) */
	X80_QOS_SVP_ACTIVE = 0x18,         /* u8 d28 */
	X80_QOS_SVP_SENTINEL = 0x19,       /* u8 d29 */
	X80_QOS_SVP_EXPIRES = 0x20,        /* u64 d30, jiffies domain */
	X80_QOS_SVP_RUNTIME_END = 0x28,    /* u64 d38, sum_exec_runtime domain */
	X80_QOS_FLAGS_D6D = 0x5d,          /* u8 d6d */
	X80_QOS_FLAGS_D6F = 0x5f,          /* u8 d6f */
	X80_QOS_SHARED_NODE = 0x90,        /* list_head da0/da8 */
	X80_QOS_BOOST_DB8 = 0xa8,          /* acquire-loaded u64 db8 */
	X80_QOS_SENTINEL_DF2 = 0xe2,       /* u8 df2 */
	X80_QOS_LAST_ACCOUNTED = 0x134,    /* u8 e44 */
	X80_QOS_FLAGS_E45 = 0x135,         /* u8 e45 */
	X80_QOS_FLOOR_E46 = 0x136,         /* u8 e46 */
	X80_QOS_FLOOR_E6B = 0x15b,         /* u8 e6b */
};

static_assert(sizeof(u8) == 1);
static_assert(sizeof(u16) == 2);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(u64) == 8);
static_assert(sizeof(unsigned long) == 8);
static_assert(sizeof(struct list_head) == 16);
static_assert(offsetof(struct task_struct, android_oem_data1) ==
	      X80_TASK_OEM_BASE);
static_assert(sizeof(((struct task_struct *)0)->android_oem_data1) ==
	      X80_TASK_OEM_SIZE);
static_assert(offsetof(struct task_struct, se.sum_exec_runtime) == 0x108);
static_assert(offsetof(struct task_struct, android_oem_data1[18]) == 0xda0);
static_assert(offsetof(struct task_struct, android_oem_data1[19]) == 0xda8);
static_assert(offsetof(struct task_struct, android_oem_data1[21]) == 0xdb8);
static_assert((X80_TASK_OEM_BASE + X80_QOS_SHARED_NODE) %
	      __alignof__(struct list_head) == 0);
static_assert(X80_QOS_SHARED_NODE + sizeof(struct list_head) <=
	      X80_TASK_OEM_SIZE);
static_assert(X80_QOS_POLICY_RESET + X80_QOS_POLICY_RESET_SIZE == 0x160);
static_assert(X80_QOS_POLICY_RESET + X80_QOS_POLICY_RESET_SIZE <=
	      X80_TASK_OEM_SIZE);
static_assert(X80_QOS_FLOOR_E6B + sizeof(u8) <=
	      X80_QOS_POLICY_RESET + X80_QOS_POLICY_RESET_SIZE);

static inline const u8 *x80_stock_task_oem(const struct task_struct *task)
{
	return (const u8 *)task->android_oem_data1;
}

static inline u8 *x80_stock_task_oem_mut(struct task_struct *task)
{
	return (u8 *)task->android_oem_data1;
}

static inline u8 x80_stock_task_read_u8(const struct task_struct *task,
				     unsigned int offset)
{
	return READ_ONCE(x80_stock_task_oem(task)[offset]);
}

/* Only call at the naturally aligned, u64-width offsets proved above. */
static inline u64 x80_stock_task_read_u64(const struct task_struct *task,
				       unsigned int offset)
{
	return READ_ONCE(*(const u64 *)(x80_stock_task_oem(task) + offset));
}

static inline u64 x80_stock_task_boost_acquire(const struct task_struct *task)
{
	return smp_load_acquire((const u64 *)(x80_stock_task_oem(task) +
					   X80_QOS_BOOST_DB8));
}

static inline struct list_head *x80_stock_task_shared_node(
						struct task_struct *task)
{
	return (struct list_head *)(x80_stock_task_oem_mut(task) +
				   X80_QOS_SHARED_NODE);
}

/*
 * Stock CMP+B.PL/B.MI tests N only, not N==V.  Unsigned subtraction first
 * avoids C signed-overflow UB, then casting reproduces the 64-bit N flag.
 * Do not replace with signed operand comparison or unsigned >=.
 */
static inline bool x80_stock_delta_nonnegative(u64 lhs, u64 rhs)
{
	return (s64)(lhs - rhs) >= 0;
}

/*
 * Exact stock generic post-dup reset, executed AFTER Android dup hooks.
 * task is unpublished. NEVER call this on an enqueued/live task: its node
 * may own a usage reference and list links. Preserve unrelated OEM bytes.
 * Replace/centralize the existing fork.c d28..e70 reset; do NOT add another.
 * This is not fork policy inheritance (fork_set_rsc_qos_level is separate).
 */
static inline void x80_stock_qos_init_new_task(struct task_struct *task)
{
	u8 *oem = x80_stock_task_oem_mut(task);

	memset(oem + X80_QOS_POLICY_RESET, 0, X80_QOS_POLICY_RESET_SIZE);
	oem[X80_QOS_SVP_SENTINEL] = 0xff;
	INIT_LIST_HEAD(x80_stock_task_shared_node(task));
	oem[X80_QOS_SENTINEL_DF2] = 0xff;
}

int get_task_qos(struct task_struct *task);
void x80_stock_sys_set_qos_level(struct task_struct *task, u8 flags, u8 set);
void x80_stock_fork_set_rsc_qos_level(struct task_struct *child);
#endif
