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
#include <linux/string.h>
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
#include <mach/mtk_rtc.h>
#include <linux/notifier.h>
#include <linux/jiffies.h>

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

/* Bring-up: every way this kernel can reset the SoC is turned into a kernel
 * panic, so MTK ipanic writes the whole log buffer to expdb (SYS_KERNEL_LOG)
 * and the RTC recovery flag makes the device land in recovery (TWRP) instead
 * of boot-looping. Covered:
 *  - kernel_restart() with any target except "recovery" (Pie init asks for
 *    "bootloader" on LOG(FATAL), "" for a plain reboot, ...)
 *  - kernel_power_off()
 *  - arch_reset() called directly by MTK drivers (charger/battery/PMIC code
 *    does arch_reset(0, NULL) without going through kernel_restart)
 *  - optionally a timed snapshot: klog_dump.panic_after=<seconds since boot>
 *    (LK drops the boot.img cmdline, so set it in CONFIG_CMDLINE, or write
 *    /sys/module/klog_dump/parameters/panic_after from init.rc).
 * The ROM boots now, so these are OFF by default: enable them for a debug
 * session with klog_dump.panic_on_reboot=1 / klog_dump.panic_on_poweroff=1
 * (CONFIG_CMDLINE) or by writing /sys/module/klog_dump/parameters/. A real
 * kernel panic still lands in recovery with the log saved by ipanic. */
static bool panic_on_reboot;
module_param(panic_on_reboot, bool, 0644);
static bool panic_on_poweroff;
module_param(panic_on_poweroff, bool, 0644);
static int panic_after;
module_param(panic_after, int, 0644);

static bool klog_dump_in_panic;

static bool klog_dump_passthrough(const char *cmd)
{
	return cmd && (!strcmp(cmd, "recovery") || !strcmp(cmd, "charger") ||
		       !strcmp(cmd, "kpoc"));
}

void klog_dump_panic_hook(const char *cmd)
{
	if (cmd && !strcmp(cmd, "power_off")) {
		if (panic_on_poweroff) {
			rtc_mark_recovery();
			panic("klog_dump: userspace requested power off");
		}
		return;
	}
	if (panic_on_reboot && !klog_dump_passthrough(cmd)) {
		rtc_mark_recovery();
		panic("klog_dump: kernel_restart(\"%s\")", cmd ? cmd : "NULL");
	}
}

/* Called first thing in arch_reset(). The panic path itself ends up here via
 * emergency_restart(), so only act when we are not already panicking. */
void klog_dump_arch_reset_hook(const char *cmd)
{
	if (klog_dump_in_panic || oops_in_progress || !panic_on_reboot ||
	    klog_dump_passthrough(cmd))
		return;
	klog_dump_in_panic = true;
	rtc_mark_recovery();
	panic("klog_dump: direct arch_reset(\"%s\")", cmd ? cmd : "NULL");
}

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

/* Called from kernel_init() right before /init runs (debug mode only). The
 * eMMC is probed asynchronously, so give it a moment first. */
void klog_dump_before_init(void)
{
	if (!enable || periodic <= 0)
		return;
	msleep(1500);
	klog_dump_to_emmc("before_init");
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

/* Bring-up: any kernel panic (not only the hooks above) should come back in
 * recovery so the ipanic record in expdb can be read without key combos. */
static int klog_dump_panic_notify(struct notifier_block *nb, unsigned long e, void *p)
{
	klog_dump_in_panic = true;
	rtc_mark_recovery();
	return NOTIFY_DONE;
}

static struct notifier_block klog_dump_panic_nb = {
	.notifier_call = klog_dump_panic_notify,
	.priority = INT_MAX,
};

static int __init klog_dump_notifier_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list, &klog_dump_panic_nb);
	return 0;
}
early_initcall(klog_dump_notifier_init);

static int klog_dump_timer_fn(void *unused)
{
	while (!kthread_should_stop()) {
		msleep(1000);
		if (panic_after > 0 &&
		    (int)(jiffies_to_msecs(jiffies - INITIAL_JIFFIES) / 1000) >= panic_after) {
			rtc_mark_recovery();
			panic("klog_dump: timed snapshot at %d s", panic_after);
		}
	}
	return 0;
}

static int __init klog_dump_periodic_init(void)
{
	kthread_run(klog_dump_timer_fn, NULL, "klog_dump_timer");
	if (enable && periodic > 0)
		kthread_run(klog_dump_periodic_fn, NULL, "klog_dump");
	return 0;
}
late_initcall_sync(klog_dump_periodic_init);
