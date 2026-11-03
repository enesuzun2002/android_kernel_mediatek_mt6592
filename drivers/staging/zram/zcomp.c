/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/lzo.h>
#ifdef CONFIG_LZ4K
#include <linux/lz4k.h>
#endif
#ifdef CONFIG_LZ4_COMPRESS
#include <linux/lz4.h>
#endif

#include "zcomp.h"

/*
 * Compression backends. Without ZSM, LZ4K is the default (as it always was
 * on this platform) and is listed first; the first entry is the default.
 */
#ifdef CONFIG_ZSM
static void *zcomp_lzo_create(void)
{
	return kzalloc(LZO1X_MEM_COMPRESS, GFP_KERNEL);
}

static void zcomp_lzo_destroy(void *private)
{
	kfree(private);
}

static int zcomp_lzo_compress(const unsigned char *src, unsigned char *dst,
		size_t *dst_len, void *private, int *checksum)
{
	return lzo1x_1_compress_zram(src, PAGE_SIZE, dst, dst_len, private,
			checksum);
}

static int zcomp_lzo_decompress(const unsigned char *src, size_t src_len,
		unsigned char *dst)
{
	size_t dst_len = PAGE_SIZE;

	return lzo1x_decompress_safe(src, src_len, dst, &dst_len);
}

static struct zcomp_backend zcomp_lzo = {
	.compress = zcomp_lzo_compress,
	.decompress = zcomp_lzo_decompress,
	.create = zcomp_lzo_create,
	.destroy = zcomp_lzo_destroy,
	.name = "lzo",
};

static struct zcomp_backend *backends[] = {
	&zcomp_lzo,
	NULL
};
#else /* !CONFIG_ZSM */

static void *zcomp_lzo_create(void)
{
	return kzalloc(LZO1X_MEM_COMPRESS, GFP_KERNEL);
}

static void zcomp_lzo_destroy(void *private)
{
	kfree(private);
}

static int zcomp_lzo_compress(const unsigned char *src, unsigned char *dst,
		size_t *dst_len, void *private)
{
	return lzo1x_1_compress(src, PAGE_SIZE, dst, dst_len, private);
}

static int zcomp_lzo_decompress(const unsigned char *src, size_t src_len,
		unsigned char *dst)
{
	size_t dst_len = PAGE_SIZE;

	return lzo1x_decompress_safe(src, src_len, dst, &dst_len);
}

static struct zcomp_backend zcomp_lzo = {
	.compress = zcomp_lzo_compress,
	.decompress = zcomp_lzo_decompress,
	.create = zcomp_lzo_create,
	.destroy = zcomp_lzo_destroy,
	.name = "lzo",
};

#ifdef CONFIG_LZ4K
static void *zcomp_lz4k_create(void)
{
	return kzalloc(LZ4K_MEM_COMPRESS, GFP_KERNEL);
}

static void zcomp_lz4k_destroy(void *private)
{
	kfree(private);
}

static int zcomp_lz4k_compress(const unsigned char *src, unsigned char *dst,
		size_t *dst_len, void *private)
{
	return lz4k_compress(src, PAGE_SIZE, dst, dst_len, private);
}

static int zcomp_lz4k_decompress(const unsigned char *src, size_t src_len,
		unsigned char *dst)
{
	size_t dst_len = PAGE_SIZE;

	return lz4k_decompress_safe(src, src_len, dst, &dst_len);
}

static struct zcomp_backend zcomp_lz4k = {
	.compress = zcomp_lz4k_compress,
	.decompress = zcomp_lz4k_decompress,
	.create = zcomp_lz4k_create,
	.destroy = zcomp_lz4k_destroy,
	.name = "lz4k",
};
#endif

#ifdef CONFIG_LZ4_COMPRESS
static void *zcomp_lz4_create(void)
{
	return kzalloc(LZ4_MEM_COMPRESS, GFP_KERNEL);
}

static void zcomp_lz4_destroy(void *private)
{
	kfree(private);
}

static int zcomp_lz4_compress(const unsigned char *src, unsigned char *dst,
		size_t *dst_len, void *private)
{
	/* return : Success if return 0 */
	return lz4_compress(src, PAGE_SIZE, dst, dst_len, private);
}

static int zcomp_lz4_decompress(const unsigned char *src, size_t src_len,
		unsigned char *dst)
{
	size_t dst_len = PAGE_SIZE;
	/* return : Success if return 0 */
	return lz4_decompress_unknownoutputsize(src, src_len, dst, &dst_len);
}

static struct zcomp_backend zcomp_lz4 = {
	.compress = zcomp_lz4_compress,
	.decompress = zcomp_lz4_decompress,
	.create = zcomp_lz4_create,
	.destroy = zcomp_lz4_destroy,
	.name = "lz4",
};
#endif

static struct zcomp_backend *backends[] = {
#ifdef CONFIG_LZ4K
	&zcomp_lz4k,
#endif
	&zcomp_lzo,
#ifdef CONFIG_LZ4_COMPRESS
	&zcomp_lz4,
#endif
	NULL
};
#endif /* CONFIG_ZSM */

static struct zcomp_backend *find_backend(const char *compress)
{
	int i = 0;

	while (backends[i]) {
		if (sysfs_streq(compress, backends[i]->name))
			break;
		i++;
	}
	return backends[i];
}

const char *zcomp_default_name(void)
{
	return backends[0]->name;
}

bool zcomp_available_algorithm(const char *comp)
{
	return find_backend(comp) != NULL;
}

/* show available compressors, the selected one in brackets */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	ssize_t sz = 0;
	int i = 0;

	while (backends[i]) {
		if (sysfs_streq(comp, backends[i]->name))
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", backends[i]->name);
		else
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", backends[i]->name);
		i++;
	}
	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

static void zcomp_strm_free(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	if (zstrm->private)
		comp->backend->destroy(zstrm->private);
	free_pages((unsigned long)zstrm->buffer, 1);
	kfree(zstrm);
}

/*
 * allocate new zcomp_strm structure with ->private initialized by
 * backend, return NULL on error
 */
static struct zcomp_strm *zcomp_strm_alloc(struct zcomp *comp)
{
	struct zcomp_strm *zstrm = kmalloc(sizeof(*zstrm), GFP_NOIO);

	if (!zstrm)
		return NULL;

	zstrm->private = comp->backend->create();
	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	zstrm->buffer = (void *)__get_free_pages(GFP_NOIO | __GFP_ZERO, 1);
	if (!zstrm->private || !zstrm->buffer) {
		zcomp_strm_free(comp, zstrm);
		zstrm = NULL;
	}
	return zstrm;
}

/*
 * get idle zcomp_strm or wait until other process release
 * (zcomp_strm_release()) one for us
 */
struct zcomp_strm *zcomp_strm_find(struct zcomp *comp)
{
	struct zcomp_strm *zstrm;

	while (1) {
		spin_lock(&comp->strm_lock);
		if (!list_empty(&comp->idle)) {
			zstrm = list_entry(comp->idle.next,
					struct zcomp_strm, list);
			list_del(&zstrm->list);
			spin_unlock(&comp->strm_lock);
			return zstrm;
		}
		/* allocate a new stream if we are below the limit */
		if (comp->avail_strm < comp->max_strm) {
			comp->avail_strm++;
			spin_unlock(&comp->strm_lock);

			zstrm = zcomp_strm_alloc(comp);
			if (!zstrm) {
				spin_lock(&comp->strm_lock);
				comp->avail_strm--;
				spin_unlock(&comp->strm_lock);
				/* let waiters retry, we may have failed */
				wake_up(&comp->strm_wait);
				/* fall back to waiting for a busy stream */
				spin_lock(&comp->strm_lock);
				if (comp->avail_strm == 0) {
					spin_unlock(&comp->strm_lock);
					return NULL;
				}
				spin_unlock(&comp->strm_lock);
				continue;
			}
			return zstrm;
		}
		/* wait until a stream is released */
		{
			DEFINE_WAIT(wait);

			prepare_to_wait(&comp->strm_wait, &wait,
					TASK_UNINTERRUPTIBLE);
			if (list_empty(&comp->idle) &&
			    comp->avail_strm >= comp->max_strm) {
				spin_unlock(&comp->strm_lock);
				schedule();
			} else {
				spin_unlock(&comp->strm_lock);
			}
			finish_wait(&comp->strm_wait, &wait);
		}
	}
}

/* add stream back to idle list and wake up waiter or free the stream */
void zcomp_strm_release(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	spin_lock(&comp->strm_lock);
	if (comp->avail_strm <= comp->max_strm) {
		list_add(&zstrm->list, &comp->idle);
		spin_unlock(&comp->strm_lock);
		wake_up(&comp->strm_wait);
		return;
	}

	/* max_strm was lowered while this stream was busy */
	comp->avail_strm--;
	spin_unlock(&comp->strm_lock);
	zcomp_strm_free(comp, zstrm);
}

/* change max_strm limit */
bool zcomp_set_max_streams(struct zcomp *comp, int num_strm)
{
	struct zcomp_strm *zstrm;

	spin_lock(&comp->strm_lock);
	if (num_strm < comp->max_strm) {
		/* free idle streams above the new limit */
		while (comp->avail_strm > num_strm &&
		       !list_empty(&comp->idle)) {
			zstrm = list_entry(comp->idle.next,
					struct zcomp_strm, list);
			list_del(&zstrm->list);
			comp->avail_strm--;
			spin_unlock(&comp->strm_lock);
			zcomp_strm_free(comp, zstrm);
			spin_lock(&comp->strm_lock);
		}
	}
	comp->max_strm = num_strm;
	spin_unlock(&comp->strm_lock);
	wake_up(&comp->strm_wait);
	return true;
}

#ifdef CONFIG_ZSM
int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		const unsigned char *src, size_t *dst_len, int *checksum)
{
	return comp->backend->compress(src, zstrm->buffer, dst_len,
			zstrm->private, checksum);
}
#else
int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		const unsigned char *src, size_t *dst_len)
{
	return comp->backend->compress(src, zstrm->buffer, dst_len,
			zstrm->private);
}
#endif

int zcomp_decompress(struct zcomp *comp, const unsigned char *src,
		size_t src_len, unsigned char *dst)
{
	return comp->backend->decompress(src, src_len, dst);
}

void zcomp_destroy(struct zcomp *comp)
{
	struct zcomp_strm *zstrm;

	while (!list_empty(&comp->idle)) {
		zstrm = list_entry(comp->idle.next, struct zcomp_strm, list);
		list_del(&zstrm->list);
		zcomp_strm_free(comp, zstrm);
	}
	kfree(comp);
}

/*
 * search available compressors for requested algorithm.
 * allocate new zcomp and initialize it. return compressing
 * backend pointer or ERR_PTR if things went bad. ERR_PTR(-EINVAL)
 * if requested algorithm is not supported, ERR_PTR(-ENOMEM) in
 * case of allocation error.
 */
struct zcomp *zcomp_create(const char *compress, int max_strm)
{
	struct zcomp *comp;
	struct zcomp_backend *backend;

	backend = find_backend(compress);
	if (!backend)
		return ERR_PTR(-EINVAL);

	comp = kzalloc(sizeof(struct zcomp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->backend = backend;
	spin_lock_init(&comp->strm_lock);
	INIT_LIST_HEAD(&comp->idle);
	init_waitqueue_head(&comp->strm_wait);
	comp->max_strm = max_strm;
	comp->avail_strm = 0;
	return comp;
}
