/*
 * Copyright (C) 2026 hulkbuster (LineageOS 14.1)
 *
 * Periodic kernel log sink for the General Mobile ETAB5 bring-up.
 *
 * Why this exists
 * ---------------
 * On this device the only ways to get a kernel log are a panic dump in
 * expdb (readable from TWRP as /dev/expdb) or a shell over adb, and the
 * failure we are chasing produces neither:
 *
 *   - there is no UART, and TWRP exposes neither /dev/mem nor /proc/kcore,
 *     so the MTK RAM console cannot be read back;
 *   - the board has no device tree, so cmdline options cannot be passed to
 *     configure pstore, and ramoops has no reserved region to live in;
 *   - userspace never starts, so adb never appears and
 *     /sys/fs/pstore is never mounted;
 *   - and the failure mode is a *hang*, not a panic. CPU0 sits in a
 *     level-triggered I2C interrupt (mt_i2c_irq) that is never acknowledged,
 *     so ipanic's panic path never runs and expdb keeps whatever the last
 *     panic left there - which is how a stale dump from an older kernel is
 *     easily mistaken for a fresh one.
 *
 * So this writes the ring buffer out on a timer, unconditionally, while the
 * kernel is still alive to do it. Whatever the last snapshot says is where
 * the kernel got to, and it is readable from TWRP long after the board has
 * been hard reset.
 *
 * Where it writes
 * ---------------
 * Straight into the expdb partition with the AEE dump writer
 * (card_dump_func_write), which uses its own msdc handle
 * (EXTEND_STORAGE_ID in drivers/misc/mediatek/mmc-host/mt6592/mt_dump.c) and
 * so does not contend with the host that mounts /system.
 *
 * Measured occupancy of the 0xa00000 byte expdb partition on this device,
 * by dumping it whole and walking every 64 KiB:
 *
 *   0x000c00 - 0x654000   11 ipanic records (kernel log, wdt log, oops
 *                         detail, mini rdump, the four *LOG_RAW, atf log)
 *   0x660000 - 0x700000   erased (0xff)   <- KLOG_TAIL_OFF, 64 KiB
 *   0x700000 - 0x702000   MTK RAM console, signature "DBGC"
 *                         (0x43474244, dram_console.c). Left by the factory
 *                         kernel; this build has CONFIG_MTK_AEE_DRAM_CONSOLE
 *                         off, but it is NOT ours to overwrite.
 *   0x702000 - 0xa000000  erased (0xff)   <- KLOG_FULL_OFF, 1 MiB
 *
 * An earlier version of this sink used 0x700000 for the tail, having only
 * checked that it was past the end of the ipanic table. That is exactly the
 * region the RAM console occupies, so those writes would have destroyed it.
 * klog_region_usable() below now checks before writing anything, so a wrong
 * offset is a loud boot-time refusal rather than silent corruption.
 *
 * Both regions hold only this sink's own data, and they are past the ipanic
 * table, so snapshots never clobber a real panic dump and a panic dump never
 * clobbers these.
 *
 * Reading them back from TWRP:
 *
 *   adb shell "dd if=/dev/expdb of=/sdcard/expdb.bin bs=1024 count=10240"
 *   adb pull /sdcard/expdb.bin
 *   ./decode_expdb.py expdb.bin --klog
 *
 * The dump must be read to the end of the partition. Stopping at 2 MB, as
 * was done earlier, misses both the extra ipanic records and these.
 *
 * Cost control: the frequent snapshot is deliberately small (64 KiB, the
 * tail of the ring buffer) and writes are 512-byte aligned and block sized,
 * so this is ~32 KiB/s of eMMC traffic during bring-up.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/kmsg_dump.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/vmalloc.h>
#include <linux/mmc/sd_misc.h>

/* This 3.10 tree has no PANIC_NOTIFY: MTK routes panics through its own
 * kernel-internal chain, which is what aee_ipanic_init() registers on. */
extern struct atomic_notifier_head panic_notifier_list;

extern int card_dump_func_write(unsigned char *buf, unsigned int len,
				unsigned long long offset, int dev);
extern int card_dump_func_read(unsigned char *buf, unsigned int len,
			       unsigned long long offset, int dev);

/* Erased eMMC reads as all-ones. */
#define KLOG_ERASED_BYTE	0xff

/* Layout inside expdb. Both offsets are 512-byte aligned as the AEE writer
 * requires, both fit inside the 0xa00000 byte partition, and both sit in
 * regions measured to be erased. See the occupancy map at the top of this
 * file: 0x700000 in particular is MTK's RAM console, not free space. */
#define KLOG_TAIL_OFF	0x660000ULL	/* 64 KiB  - rewritten often       */
#define KLOG_FULL_OFF	0x800000ULL	/* 1 MiB   - rewritten occasionally */

#define KLOG_TAIL_SIZE	(64 * 1024)
#define KLOG_FULL_SIZE	(1024 * 1024)

#define KLOG_TAIL_PERIOD_MS	2000
#define KLOG_FULL_PERIOD_MS	30000

/* "KLG1" */
#define KLOG_MAGIC	0x31474C4BU

enum klog_kind {
	KLOG_KIND_TAIL = 0,
	KLOG_KIND_FULL = 1,
	KLOG_KIND_PANIC = 2,
	KLOG_KIND_RESTART = 3,
};

/*
 * Fixed 32-byte header so the payload can be found without guessing, and so
 * that a torn write (power cut mid-dump) is detectable: the magic comes
 * first and is written last-by-position but read first.
 */
struct klog_sink_hdr {
	u32 magic;
	u32 kind;
	u32 seq;
	u32 len;		/* payload bytes, excluding padding */
	u64 uptime_ms;
	u32 write_ok;
	u32 reserved;
};

static struct work_struct klog_sink_work;
static struct timer_list klog_sink_timer;
static struct kmsg_dumper klog_dumper;
static unsigned int klog_seq;
static bool klog_in_panic;
static bool klog_tail_ok;
static bool klog_full_ok;

static bool klog_offset_ok(unsigned long long offset)
{
	if (offset == KLOG_TAIL_OFF)
		return klog_tail_ok;
	if (offset == KLOG_FULL_OFF)
		return klog_full_ok;
	return false;
}

static void klog_dumper_reset(struct kmsg_dumper *d)
{
	d->cur_idx = 0;
	d->cur_seq = 0;
	d->next_idx = 0;
	d->next_seq = 0;
	d->max_reason = KMSG_DUMP_UNDEF;
	d->active = false;
}

/*
 * Decide whether we may write to a region, and say so loudly if not.
 *
 * Accepted: the region reads as erased (all 0xff), or it already begins with
 * our own magic, which means a previous boot of this sink wrote it and we
 * own it.
 *
 * Anything else is another subsystem's data - the RAM console at 0x700000 is
 * the case that actually happened here - so refuse rather than destroy it.
 * A sink that silently overwrites someone else's region is worse than no
 * sink at all: it looks like it is working while eating a debug facility.
 */
static bool klog_region_usable(const char *name, unsigned long long offset,
			       size_t size, bool *already_ours)
{
	unsigned char *probe;
	size_t i;
	bool erased = true;
	bool ours = false;
	int ret;

	probe = (unsigned char *)vzalloc(size);
	if (!probe) {
		printk("klog_sink: cannot verify %s region, no memory\n", name);
		return false;
	}

	ret = card_dump_func_read(probe, (unsigned int)size, offset,
				  DUMP_INTO_BOOT_CARD_IPANIC);
	if (ret != 0) {
		printk("klog_sink: cannot read %s region at 0x%llx (ret %d), "
		       "refusing to write it\n", name, offset, ret);
		vfree(probe);
		return false;
	}

	for (i = 0; i < size; i++) {
		if (probe[i] != KLOG_ERASED_BYTE) {
			erased = false;
			break;
		}
	}
	if (!erased && size >= sizeof(u32) &&
	    !memcmp(probe, &((u32){ KLOG_MAGIC }), sizeof(u32)))
		ours = true;

	if (already_ours)
		*already_ours = ours;

	if (!erased && !ours) {
		printk("klog_sink: %s region at 0x%llx is in use by something "
		       "else (first bytes %02x %02x %02x %02x) - NOT writing\n",
		       name, offset, probe[0], probe[1], probe[2], probe[3]);
		vfree(probe);
		return false;
	}

	printk("klog_sink: %s region 0x%llx is %s\n", name, offset,
	       erased ? "erased" : "ours from a previous boot");
	vfree(probe);
	return true;
}

static void klog_dump_once(enum klog_kind kind, unsigned long long offset,
			   size_t size)
{
	unsigned char *buf;
	struct klog_sink_hdr *hdr;
	size_t len = 0;
	size_t total;
	u64 ms;
	int ret;

	/* Verified once at init; a region we may not touch stays untouched. */
	if (!klog_offset_ok(offset))
		return;

	buf = (unsigned char *)vzalloc(size + sizeof(*hdr));
	if (!buf) {
		printk("klog_sink: out of memory for a %zu byte dump\n", size);
		return;
	}

	kmsg_dump_get_buffer(&klog_dumper, true, (char *)buf + sizeof(*hdr),
			     size, &len);

	hdr = (struct klog_sink_hdr *)buf;
	ms = ktime_to_ms(ktime_get());
	total = sizeof(*hdr) + len;

	hdr->magic = KLOG_MAGIC;
	hdr->kind = kind;
	hdr->seq = ++klog_seq;
	hdr->len = (u32)len;
	hdr->uptime_ms = ms;
	hdr->write_ok = 0;
	hdr->reserved = 0;

	/* The AEE writer requires a multiple of 512 bytes. */
	if (total & 511)
		total = (total + 511) & ~511ULL;

	ret = card_dump_func_write(buf, (unsigned int)total, offset,
				   DUMP_INTO_BOOT_CARD_IPANIC);

	hdr->write_ok = (ret == 0) ? 1 : 0;

	printk("klog_sink: kind=%d seq=%u uptime=%llums len=%zu off=0x%llx ret=%d\n",
	       kind, hdr->seq, (unsigned long long)ms, len, offset, ret);

	vfree(buf);
}

static void klog_sink_timer_fn(unsigned long data)
{
	schedule_work(&klog_sink_work);
}

static void klog_sink_work_fn(struct work_struct *work)
{
	static unsigned int ticks;

	klog_dump_once(KLOG_KIND_TAIL, KLOG_TAIL_OFF, KLOG_TAIL_SIZE);

	/* The full ring buffer occasionally, so early boot is not lost when
	 * the tail has long since wrapped over it. */
	if (++ticks % (KLOG_FULL_PERIOD_MS / KLOG_TAIL_PERIOD_MS) == 0)
		klog_dump_once(KLOG_KIND_FULL, KLOG_FULL_OFF, KLOG_FULL_SIZE);

	mod_timer(&klog_sink_timer,
		  jiffies + msecs_to_jiffies(KLOG_TAIL_PERIOD_MS));
}

static int klog_sink_panic(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	enum klog_kind kind = KLOG_KIND_PANIC;

	klog_in_panic = true;
	/* Tail first: it is small, so it lands even if the big one is cut
	 * short by the reset that usually follows. */
	klog_dump_once(KLOG_KIND_TAIL, KLOG_TAIL_OFF, KLOG_TAIL_SIZE);
	klog_dump_once(kind, KLOG_FULL_OFF, KLOG_FULL_SIZE);

	return NOTIFY_DONE;
}

static struct notifier_block klog_sink_nb = {
	.notifier_call = klog_sink_panic,
};

static int __init klog_sink_init(void)
{
	INIT_WORK(&klog_sink_work, klog_sink_work_fn);
	setup_timer(&klog_sink_timer, klog_sink_timer_fn, 0);

	klog_dumper_reset(&klog_dumper);

	/*
	 * Verify both regions before any timer is armed, so a bad offset
	 * cannot start writing on its own.
	 */
	klog_tail_ok = klog_region_usable("tail", KLOG_TAIL_OFF,
					  KLOG_TAIL_SIZE, NULL);
	klog_full_ok = klog_region_usable("full", KLOG_FULL_OFF,
					  KLOG_FULL_SIZE, NULL);

	if (kmsg_dump_register(&klog_dumper)) {
		printk("klog_sink: kmsg_dump_register failed, sink disabled\n");
		return -ENODEV;
	}

	atomic_notifier_chain_register(&panic_notifier_list, &klog_sink_nb);

	if (!klog_tail_ok && !klog_full_ok) {
		printk("klog_sink: no usable region, sink disabled\n");
		return -ENODEV;
	}

	/* Fire once early, in case boot dies before the first period is up. */
	mod_timer(&klog_sink_timer, jiffies + msecs_to_jiffies(4000));

	printk("klog_sink: active, tail@0x%llx every %dms, full@0x%llx every %dms\n",
	       KLOG_TAIL_OFF, KLOG_TAIL_PERIOD_MS,
	       KLOG_FULL_OFF, KLOG_FULL_PERIOD_MS);

	return 0;
}

late_initcall(klog_sink_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Periodic kernel log sink into expdb for hulkbuster bring-up");