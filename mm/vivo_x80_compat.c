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
#include <linux/types.h>
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

/* The vendor scheduler/QoS extensions are unavailable in this kernel. */
bool is_task_crucial(struct task_struct *task)
{
	return false;
}
EXPORT_SYMBOL(is_task_crucial);

int get_task_qos(struct task_struct *task)
{
	return 0;
}
EXPORT_SYMBOL(get_task_qos);

/* zcache state notification is optional; leave it as a no-op. */
void write_zcache_state(int enabled)
{
}
EXPORT_SYMBOL(write_zcache_state);

/* Vendor policy/data knobs: disabled/empty by default. */
int rsc_qos_enable;
EXPORT_SYMBOL(rsc_qos_enable);

int sysctl_allow_alloc_from_thppool;
EXPORT_SYMBOL(sysctl_allow_alloc_from_thppool);

/* Incremented with a 32-bit atomic (LDADDL in vendor system_heap.ko). */
atomic_t nr_dmapages_in_thppool = ATOMIC_INIT(0);
EXPORT_SYMBOL(nr_dmapages_in_thppool);

/* Real 32-bit pool accounting; no pages enter the disabled optional pool. */
atomic_t nr_ezrampages_in_thppool = ATOMIC_INIT(0);
EXPORT_SYMBOL(nr_ezrampages_in_thppool);
