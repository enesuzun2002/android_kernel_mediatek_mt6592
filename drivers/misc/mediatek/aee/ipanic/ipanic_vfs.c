#include <linux/file.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <uapi/asm-generic/fcntl.h>
#include <linux/err.h>
#include "ipanic.h"


struct file *expdb_open(void)
{
	static struct file *filp_expdb;

	/* Return the cached handle only if it is a real one. */
	if (filp_expdb && !IS_ERR(filp_expdb))
		return filp_expdb;

	/*
	 * Never cache the failure. filp_open() returns an ERR_PTR, which is
	 * non-NULL, so caching it made this function non-NULL on the next
	 * call and it never retried. Every caller's retry loop then spun on
	 * the same stale error - which is why the working kernel logs
	 * "ram_console: open expdb partition error [-2]" over and over for
	 * 30 seconds instead of ever succeeding.
	 *
	 * Note that -ENOENT here is not a missing driver: nothing registers
	 * a misc device named "expdb" and CONFIG_DEVTMPFS is off, so the node
	 * only exists once userspace creates it. Kernel code that needs expdb
	 * before userspace should use card_dump_func_read()/write() instead,
	 * which drive the msdc host directly and need no node at all.
	 */
	filp_expdb = filp_open(AEE_EXPDB_PATH, O_RDWR, 0);
	if (IS_ERR(filp_expdb)) {
		LOGD("filp_open(%s) for aee failed (%ld)\n", AEE_EXPDB_PATH, PTR_ERR(filp_expdb));
	}
	return filp_expdb;
}

ssize_t expdb_write(struct file *filp, const char *buf, size_t len, loff_t off)
{
	return kernel_write(filp, buf, len, off);
}

ssize_t expdb_read(struct file *filp, char *buf, size_t len, loff_t off)
{
	return kernel_read(filp, off, buf, len);
}
  
char *expdb_read_size(int off, int len)
{
	int ret;
	struct file *filp;
	char *data;
	int timeout = 0;
	do {
		filp = expdb_open();
		if (timeout++ > 3) {
			LOGE("open expdb partition fail [%ld]!\n", PTR_ERR(filp));
			return NULL;
		}
		msleep(500);
	} while (IS_ERR(filp));
	data = kzalloc(len, GFP_KERNEL);
	ret = kernel_read(filp, off, data, len);
	//fput(filp);
	if (IS_ERR(ERR_PTR(ret))) {
		kfree(data);
		data = NULL;
		LOGE("read from expdb fail [%d]!\n", ret);
	}
	return data;
}
