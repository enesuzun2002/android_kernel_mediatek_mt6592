/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_

#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/list.h>

struct zcomp_strm {
	/* compression/decompression buffer */
	void *buffer;
	/*
	 * The private data of the compression stream, only compression
	 * stream backend can touch this (e.g. compression algorithm
	 * working memory)
	 */
	void *private;
	/* used in multi stream backend, protected by backend strm_lock */
	struct list_head list;
};

/* static compression backend */
struct zcomp_backend {
#ifdef CONFIG_ZSM
	int (*compress)(const unsigned char *src, unsigned char *dst,
			size_t *dst_len, void *private, int *checksum);
#else
	int (*compress)(const unsigned char *src, unsigned char *dst,
			size_t *dst_len, void *private);
#endif

	int (*decompress)(const unsigned char *src, size_t src_len,
			unsigned char *dst);

	void *(*create)(void);
	void (*destroy)(void *private);

	const char *name;
};

/* dynamic per-device compression frontend */
struct zcomp {
	struct list_head idle;
	struct zcomp_backend *backend;
	spinlock_t strm_lock;
	/* the number of allocated compression streams */
	int avail_strm;
	/* limit of allocated compression streams */
	int max_strm;
	/* wait for an idle stream when all of them are busy */
	wait_queue_head_t strm_wait;
};

/* default (first) backend in the list is used when none is selected */
const char *zcomp_default_name(void);
ssize_t zcomp_available_show(const char *comp, char *buf);
bool zcomp_available_algorithm(const char *comp);

struct zcomp *zcomp_create(const char *comp, int max_strm);
void zcomp_destroy(struct zcomp *comp);

struct zcomp_strm *zcomp_strm_find(struct zcomp *comp);
void zcomp_strm_release(struct zcomp *comp, struct zcomp_strm *zstrm);

#ifdef CONFIG_ZSM
int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		const unsigned char *src, size_t *dst_len, int *checksum);
#else
int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		const unsigned char *src, size_t *dst_len);
#endif

int zcomp_decompress(struct zcomp *comp, const unsigned char *src,
		size_t src_len, unsigned char *dst);

bool zcomp_set_max_streams(struct zcomp *comp, int num_strm);
#endif /* _ZCOMP_H_ */
