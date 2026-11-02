/*
 * Dynamic fsync: make fsync()/fdatasync() a no-op while the screen is on and
 * flush all dirty data when the screen turns off, on reboot/poweroff and on
 * panic.
 *
 * Based on the dynamic fsync driver by Paul Reioux (faux123) and Dave
 * Burke's earlier proof of concept; adapted for the MediaTek 3.10 kernel.
 *
 * Runtime switch: /sys/kernel/dyn_fsync/Dyn_fsync_active (default 0 = off)
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/syscalls.h>
#include <linux/writeback.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/earlysuspend.h>
#include <linux/dyn_sync_cntrl.h>

#define DYN_FSYNC_VERSION "1.0"

/* off by default; init.rc enables it with the sysfs switch */
bool dyn_fsync_active __read_mostly;
/* the screen is on until the first early suspend */
bool dyn_fsync_screen_on __read_mostly = true;

static void dyn_fsync_flush_work(struct work_struct *work)
{
	/* full, blocking flush of every filesystem and block device */
	sys_sync();
}
static DECLARE_WORK(dyn_fsync_flush, dyn_fsync_flush_work);

static void dyn_fsync_early_suspend(struct early_suspend *h)
{
	/* from now on fsync is honoured again, then push out what was skipped */
	dyn_fsync_screen_on = false;
	smp_wmb();
	if (dyn_fsync_active)
		schedule_work(&dyn_fsync_flush);
}

static void dyn_fsync_late_resume(struct early_suspend *h)
{
	dyn_fsync_screen_on = true;
}

static struct early_suspend dyn_fsync_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = dyn_fsync_early_suspend,
	.resume = dyn_fsync_late_resume,
};

/* reboot/poweroff/halt/restart2: sleeping is allowed here */
static int dyn_fsync_reboot_notify(struct notifier_block *nb,
				   unsigned long code, void *unused)
{
	/* make every later fsync real, then flush synchronously */
	dyn_fsync_active = false;
	smp_wmb();
	sys_sync();
	return NOTIFY_DONE;
}

static struct notifier_block dyn_fsync_reboot_nb = {
	.notifier_call = dyn_fsync_reboot_notify,
	.priority = INT_MAX,	/* before anything else tears devices down */
};

/* panic: atomic context, best effort only (queues the emergency sync) */
static int dyn_fsync_panic_notify(struct notifier_block *nb,
				  unsigned long code, void *unused)
{
	dyn_fsync_active = false;
	smp_wmb();
	emergency_sync();
	return NOTIFY_DONE;
}

static struct notifier_block dyn_fsync_panic_nb = {
	.notifier_call = dyn_fsync_panic_notify,
	.priority = INT_MAX,
};

static ssize_t dyn_fsync_active_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", dyn_fsync_active ? 1 : 0);
}

static ssize_t dyn_fsync_active_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned int data;

	if (sscanf(buf, "%u", &data) != 1 || data > 1)
		return -EINVAL;

	if (data == 1) {
		dyn_fsync_active = true;
		pr_info("dyn_fsync: active\n");
	} else {
		/* make later fsyncs real first, then flush what was skipped */
		dyn_fsync_active = false;
		smp_wmb();
		schedule_work(&dyn_fsync_flush);
		pr_info("dyn_fsync: inactive\n");
	}
	return count;
}

static struct kobj_attribute dyn_fsync_active_attribute =
	__ATTR(Dyn_fsync_active, 0644, dyn_fsync_active_show,
	       dyn_fsync_active_store);

static ssize_t dyn_fsync_version_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "version: %s\n", DYN_FSYNC_VERSION);
}

static struct kobj_attribute dyn_fsync_version_attribute =
	__ATTR(Dyn_fsync_version, 0444, dyn_fsync_version_show, NULL);

static ssize_t dyn_fsync_screen_on_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", dyn_fsync_screen_on ? 1 : 0);
}

static struct kobj_attribute dyn_fsync_screen_on_attribute =
	__ATTR(Dyn_fsync_screen_on, 0444, dyn_fsync_screen_on_show, NULL);

static struct attribute *dyn_fsync_active_attrs[] = {
	&dyn_fsync_active_attribute.attr,
	&dyn_fsync_version_attribute.attr,
	&dyn_fsync_screen_on_attribute.attr,
	NULL,
};

static struct attribute_group dyn_fsync_active_attr_group = {
	.attrs = dyn_fsync_active_attrs,
};

static struct kobject *dyn_fsync_kobj;

static int __init dyn_fsync_init(void)
{
	int sysfs_result;

	dyn_fsync_kobj = kobject_create_and_add("dyn_fsync", kernel_kobj);
	if (!dyn_fsync_kobj) {
		pr_err("dyn_fsync: kobject create failed\n");
		return -ENOMEM;
	}

	sysfs_result = sysfs_create_group(dyn_fsync_kobj,
					  &dyn_fsync_active_attr_group);
	if (sysfs_result) {
		pr_err("dyn_fsync: sysfs create failed\n");
		kobject_put(dyn_fsync_kobj);
		return sysfs_result;
	}

	register_early_suspend(&dyn_fsync_early_suspend_handler);
	register_reboot_notifier(&dyn_fsync_reboot_nb);
	atomic_notifier_chain_register(&panic_notifier_list,
				       &dyn_fsync_panic_nb);

	pr_info("dyn_fsync: initialized, default off\n");
	return 0;
}
late_initcall(dyn_fsync_init);
