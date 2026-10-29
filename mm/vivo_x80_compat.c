// SPDX-License-Identifier: GPL-2.0
/*
 * Compatibility exports for the vivo vendor modules shipped with the X80.
 *
 * The Neo7 5.10.66 source does not contain several vivo-private helpers that
 * the X80 vendor ramdisk modules import.  Keep these fallbacks deliberately
 * conservative: disable the optional paths and let the callers use their
 * normal fallback implementations.
 */
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/types.h>

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

int nr_dmapages_in_thppool;
EXPORT_SYMBOL(nr_dmapages_in_thppool);
