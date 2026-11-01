/*
 * hulkbuster: save the kernel log to eMMC on every reboot request.
 *
 * The E-Tab5 has no accessible UART, the stock recovery runs a 3.4 kernel
 * that cannot read this kernel's RAM console, and MTK's ipanic only fires on
 * kernel panics. When userspace (e.g. a failing Pie init) asks for a reboot,
 * kernel_restart() calls klog_dump_to_emmc() first, which writes the tail of
 * the log buffer to the unused last 1 MiB of the expdb partition. Any
 * recovery can then read it back:
 *
 *   dd if=/dev/block/mmcblk0 bs=1048576 skip=79 count=1 of=/tmp/klog.bin
 *
 * Layout at that offset: "KLOGDUMP" magic, u32 length, u32 boot counter,
 * then the text. Disable with klog_dump.enable=0 on the kernel cmdline.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/kmsg_dump.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/init.h>

/* expdb starts at 0x4020000 and is 0xfe0000 long on mmcblk0 (dumchar_info);
 * AEE records use the first ~8 MiB, the last MiB is never written. */
#define KLOG_DUMP_OFFSET	0x4F00000
#define KLOG_DUMP_SIZE		0x100000
#define KLOG_DUMP_HDR		16
#define KLOG_DUMP_NODE		"/dev/.klogdump_mmcblk0"

static bool enable = true;
module_param(enable, bool, 0644);

/* Debug: also rewrite the dump once a second for this many seconds after
 * boot, so a hard hang that ends in a watchdog reset still leaves a log. */
static int periodic;
module_param(periodic, int, 0444);

void klog_dump_to_emmc(const char *reason)
{
	struct kmsg_dumper dumper = { .active = true };
	struct file *filp;
	char *buf;
	size_t len = 0;
	mm_segment_t old_fs;
	static u32 seq;

	if (!enable)
		return;

	if (reason)
		pr_emerg("klog_dump: saving kernel log to expdb (%s)\n", reason);

	buf = vmalloc(KLOG_DUMP_SIZE);
	if (!buf)
		return;
	memset(buf, 0, KLOG_DUMP_SIZE);

	kmsg_dump_rewind(&dumper);
	kmsg_dump_get_buffer(&dumper, true, buf + KLOG_DUMP_HDR,
			     KLOG_DUMP_SIZE - KLOG_DUMP_HDR, &len);
	memcpy(buf, "KLOGDUMP", 8);
	memcpy(buf + 8, &len, 4);
	seq++;
	memcpy(buf + 12, &seq, 4);

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	/* Early in boot (Pie first stage) there may be no mmcblk0 node yet. */
	sys_mknod(KLOG_DUMP_NODE, S_IFBLK | 0600, new_encode_dev(MKDEV(179, 0)));
	set_fs(old_fs);

	filp = filp_open(KLOG_DUMP_NODE, O_WRONLY | O_LARGEFILE, 0);
	if (IS_ERR(filp)) {
		pr_emerg("klog_dump: open failed %ld\n", PTR_ERR(filp));
		goto out;
	}
	if (kernel_write(filp, buf, KLOG_DUMP_SIZE, KLOG_DUMP_OFFSET) !=
	    KLOG_DUMP_SIZE)
		pr_emerg("klog_dump: write failed\n");
	vfs_fsync(filp, 0);
	filp_close(filp, NULL);
out:
	vfree(buf);
}

static int klog_dump_periodic_fn(void *unused)
{
	int i;

	for (i = 0; i < periodic && !kthread_should_stop(); i++) {
		msleep(1000);
		klog_dump_to_emmc(NULL);
	}
	return 0;
}

static int __init klog_dump_periodic_init(void)
{
	if (enable && periodic > 0)
		kthread_run(klog_dump_periodic_fn, NULL, "klog_dump");
	return 0;
}
late_initcall_sync(klog_dump_periodic_init);
