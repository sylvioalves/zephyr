/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_HEAP_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_HEAP_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/math_extras.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/app_platform/app_services.h>
#include <string.h>

/**
 * @brief Per-application heap
 * @defgroup app_platform_heap Per-application heap
 * @ingroup app_platform
 * @{
 *
 * An application that declares a heap size is given a region of internal RAM,
 * mapped into its own memory domain, for as long as it is loaded. Before the
 * application runs, the platform zeroes the region and initializes a heap at
 * its start. From then on only the application touches the region.
 *
 * The functions below run the allocator in the calling thread, so under user
 * mode a corrupted heap faults the application and nothing else. The threads
 * of one application share the heap through a lock word in the region. A
 * thread that finds the lock taken yields, and sleeps once yielding has not
 * helped, so a lower-priority thread holding it gets to run. Each call looks
 * the region up with @ref app_svc_heap_info, since static data of a built-in
 * application is outside its memory domain and a cache in the data of a
 * loadable one would cost it a memory partition. The same functions work
 * without user mode.
 */

/** Start of a heap region. The heap memory follows it. */
struct app_heap_region {
	atomic_t lock;        /**< taken by an application thread inside the allocator */
	struct sys_heap heap; /**< heap over the rest of the region */
};

/** @cond INTERNAL_HIDDEN */

#define APP_HEAP_LOCK_YIELDS 8U

static inline struct app_heap_region *app_heap_region_get(void)
{
	uintptr_t base;
	size_t size;

	if (app_svc_heap_info(&base, &size) != 0) {
		return NULL;
	}
	return (struct app_heap_region *)base;
}

static inline void app_heap_lock(struct app_heap_region *r)
{
	for (uint32_t n = 0U; !atomic_cas(&r->lock, 0, 1); n++) {
		if (n < APP_HEAP_LOCK_YIELDS) {
			k_yield();
		} else {
			k_msleep(1);
		}
	}
}

static inline void app_heap_unlock(struct app_heap_region *r)
{
	(void)atomic_clear(&r->lock);
}

/** @endcond */

/**
 * @brief Allocate memory from the calling application's heap.
 *
 * @param size Bytes to allocate.
 *
 * @return The memory on success.
 * @retval NULL The application has no heap, or the heap has no room.
 */
static inline void *app_malloc(size_t size)
{
	struct app_heap_region *r = app_heap_region_get();
	void *p;

	if (r == NULL) {
		return NULL;
	}
	app_heap_lock(r);
	p = sys_heap_alloc(&r->heap, size);
	app_heap_unlock(r);
	return p;
}

/**
 * @brief Allocate zeroed memory for an array from the calling application's
 *        heap.
 *
 * @param nmemb Number of elements.
 * @param size  Bytes per element.
 *
 * @return The zeroed memory on success.
 * @retval NULL The application has no heap, the heap has no room, or
 *         @p nmemb times @p size overflows.
 */
static inline void *app_calloc(size_t nmemb, size_t size)
{
	size_t bytes;
	void *p;

	if (size_mul_overflow(nmemb, size, &bytes)) {
		return NULL;
	}
	p = app_malloc(bytes);
	if (p != NULL) {
		(void)memset(p, 0, bytes);
	}
	return p;
}

/**
 * @brief Allocate aligned memory from the calling application's heap.
 *
 * @param align Alignment in bytes, a power of two.
 * @param size  Bytes to allocate.
 *
 * @return The memory on success.
 * @retval NULL The application has no heap, or the heap has no room.
 */
static inline void *app_aligned_alloc(size_t align, size_t size)
{
	struct app_heap_region *r = app_heap_region_get();
	void *p;

	if (r == NULL) {
		return NULL;
	}
	app_heap_lock(r);
	p = sys_heap_aligned_alloc(&r->heap, align, size);
	app_heap_unlock(r);
	return p;
}

/**
 * @brief Resize memory allocated from the calling application's heap.
 *
 * A NULL @p ptr allocates, a zero @p size frees @p ptr. The contents are kept
 * up to the smaller of the two sizes.
 *
 * @param ptr  Memory from this heap, or NULL.
 * @param size New size in bytes.
 *
 * @return The resized memory on success.
 * @retval NULL @p size is 0, the application has no heap, or the heap has no
 *         room, in which case @p ptr is left unchanged.
 */
static inline void *app_realloc(void *ptr, size_t size)
{
	struct app_heap_region *r = app_heap_region_get();
	void *p;

	if (r == NULL) {
		return NULL;
	}
	app_heap_lock(r);
	p = sys_heap_realloc(&r->heap, ptr, size);
	app_heap_unlock(r);
	return p;
}

/**
 * @brief Free memory allocated from the calling application's heap.
 *
 * @param ptr Memory from this heap, or NULL.
 */
static inline void app_free(void *ptr)
{
	struct app_heap_region *r;

	if (ptr == NULL) {
		return;
	}
	r = app_heap_region_get();
	if (r == NULL) {
		return;
	}
	app_heap_lock(r);
	sys_heap_free(&r->heap, ptr);
	app_heap_unlock(r);
}

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_HEAP_H_ */
