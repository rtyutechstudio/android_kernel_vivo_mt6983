// SPDX-License-Identifier: GPL-2.0
/* Two original control owners, real formats/0660 names from stock metadata. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/vivo_x80_rsc_base.h>
#include "x80_stock_qos_enqueue.h"

static ssize_t qos_reenqueue_show(struct kobject *kobj,
			struct kobj_attribute *attribute, char *buffer)
{
	return scnprintf(buffer, PAGE_SIZE, "%u\n", READ_ONCE(rsc_qos_reenqueue_time));
}
static ssize_t qos_reenqueue_store(struct kobject *kobj,
		struct kobj_attribute *attribute, const char *buffer, size_t count)
{
	u32 value = 0;

	/* Original %u sscanf permits trailing text and unsigned-width wrapping. */
	if (sscanf(buffer, "%u", &value) != 1)
		return -EINVAL;
	WRITE_ONCE(rsc_qos_reenqueue_time, value);
	return count;
}
static ssize_t svp_min_sched_delay_show(struct kobject *kobj,
			struct kobj_attribute *attribute, char *buffer)
{
	return scnprintf(buffer, PAGE_SIZE, "%d\n",
			 READ_ONCE(svp_min_sched_delay_granularity));
}
static ssize_t svp_min_sched_delay_store(struct kobject *kobj,
		struct kobj_attribute *attribute, const char *buffer, size_t count)
{
	int value, result = kstrtoint(buffer, 10, &value);

	if (result)
		return result;
	if ((unsigned int)value > 16)
		return -EINVAL;
	WRITE_ONCE(svp_min_sched_delay_granularity, value);
	return count;
}
static struct kobj_attribute x80_qos_reenqueue_attribute =
	__ATTR(qos_reenqueue, 0660, qos_reenqueue_show, qos_reenqueue_store);
static struct kobj_attribute x80_svp_min_sched_delay_attribute =
	__ATTR(svp_min_sched_delay_granularity, 0660,
	       svp_min_sched_delay_show, svp_min_sched_delay_store);
static struct attribute *x80_stock_qos_knob_attributes[] = {
	&x80_qos_reenqueue_attribute.attr,
	&x80_svp_min_sched_delay_attribute.attr,
	NULL,
};
static const struct attribute_group x80_stock_qos_knob_group = {
	.attrs = x80_stock_qos_knob_attributes,
};
/* Existing RSC owner calls after permanent root creation; no second root. */
int x80_stock_qos_knobs_control_init(struct kobject *root);
int x80_stock_qos_knobs_control_init(struct kobject *root)
{
	int ret;
	unsigned int i;
	if (!root)
		return -ENODEV;
	ret = sysfs_create_group(root, &x80_stock_qos_knob_group);
	if (ret)
		return ret;
	for (i = 0; x80_stock_qos_knob_attributes[i]; ++i) {
		ret = rsc_base_chown_attr(root, x80_stock_qos_knob_attributes[i]);
		if (ret) {
			sysfs_remove_group(root, &x80_stock_qos_knob_group);
			return ret;
		}
	}
	return 0;
}
