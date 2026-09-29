/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Per-application heap regions. A region is carved from a pool of internal RAM
 * when an application is launched, zeroed, set up as a heap and, with user
 * mode, mapped into the application's memory domain before its first thread
 * starts. The region is zeroed and returned to the pool when the application
 * is removed. The platform writes the heap only while no thread of the
 * application runs.
 */

#include <zephyr/app_platform/app_heap.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#include "app_grants.h"

#define POOL_SIZE CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE

#if POOL_SIZE > 0

BUILD_ASSERT(CONFIG_SYS_HEAP_ARRAY_SIZE == 0,
	     "per-application heaps need CONFIG_SYS_HEAP_ARRAY_SIZE=0");
BUILD_ASSERT(!IS_ENABLED(CONFIG_USERSPACE) || !IS_ENABLED(CONFIG_SYS_HEAP_LISTENER),
	     "per-application heaps in user mode need CONFIG_SYS_HEAP_LISTENER=n");
BUILD_ASSERT(!IS_ENABLED(CONFIG_USERSPACE) || !IS_ENABLED(CONFIG_SYS_HEAP_THREAD_STATS),
	     "per-application heaps in user mode need CONFIG_SYS_HEAP_THREAD_STATS=n");

#define HEAP_MAX ((size_t)CONFIG_APP_PLATFORM_APP_HEAP_MAX_BYTES)

/* Smallest region, and with an MPU or PMP the first power of two tried. */
#if defined(CONFIG_USERSPACE) && defined(CONFIG_MMU)
#define REGION_MIN ((size_t)CONFIG_MMU_PAGE_SIZE)
#elif defined(CONFIG_USERSPACE) && defined(CONFIG_RISCV_PMP)
#define REGION_MIN ((size_t)MAX(256U, CONFIG_PMP_GRANULARITY))
#else
#define REGION_MIN 256U
#endif

struct heap_slot {
	void *base;
	size_t size;
#ifdef CONFIG_USERSPACE
	struct k_mem_partition part;
#endif
};

static struct heap_slot slots[MAX_APPS];
K_HEAP_DEFINE(app_heap_pool, POOL_SIZE);

/* Size and alignment of a region holding @p heap_size heap bytes, as the
 * memory protection hardware maps it: whole pages with an MMU, a naturally
 * aligned power of two with an MPU or PMP. The caller bounds heap_size by
 * HEAP_MAX, so the search for a power of two ends.
 */
static size_t region_size(size_t heap_size, size_t *align)
{
	size_t want = MAX(heap_size + sizeof(struct app_heap_region), REGION_MIN);

#if defined(CONFIG_USERSPACE) && defined(CONFIG_MMU)
	*align = CONFIG_MMU_PAGE_SIZE;
	return ROUND_UP(want, CONFIG_MMU_PAGE_SIZE);
#elif defined(CONFIG_USERSPACE)
	size_t p = REGION_MIN;

	while (p < want) {
		p <<= 1;
	}
	*align = p;
	return p;
#else
	*align = sizeof(uint64_t);
	return ROUND_UP(want, sizeof(uint64_t));
#endif
}

static void region_init(struct heap_slot *s)
{
	struct app_heap_region *r = s->base;

	(void)memset(s->base, 0, s->size);
	sys_heap_init(&r->heap, (uint8_t *)s->base + sizeof(*r), s->size - sizeof(*r));
}

int app_platform_heap_carve(int id, struct k_mem_domain *dom, size_t heap_size)
{
	struct heap_slot *s = &slots[id];
	size_t align;
	size_t size;

	if (heap_size == 0U) {
		return 0;
	}
	if (heap_size > HEAP_MAX) {
		return -E2BIG;
	}

	size = region_size(heap_size, &align);
	s->base = k_heap_aligned_alloc(&app_heap_pool, align, size, K_NO_WAIT);
	if (s->base == NULL) {
		return -ENOMEM;
	}
	s->size = size;
	region_init(s);

#ifdef CONFIG_USERSPACE
	s->part = (struct k_mem_partition){
		.start = (uintptr_t)s->base,
		.size = size,
		.attr = K_MEM_PARTITION_P_RW_U_RW,
	};

	int rc = k_mem_domain_add_partition(dom, &s->part);

	if (rc != 0) {
		k_heap_free(&app_heap_pool, s->base);
		*s = (struct heap_slot){0};
		return rc;
	}
#else
	ARG_UNUSED(dom);
#endif
	return 0;
}

void app_platform_heap_reset(int id)
{
	if (slots[id].base != NULL) {
		region_init(&slots[id]);
	}
}

void app_platform_heap_release(int id, struct k_mem_domain *dom)
{
	struct heap_slot *s = &slots[id];

	if (s->base == NULL) {
		return;
	}
#ifdef CONFIG_USERSPACE
	(void)k_mem_domain_remove_partition(dom, &s->part);
#else
	ARG_UNUSED(dom);
#endif
	(void)memset(s->base, 0, s->size);
	k_heap_free(&app_heap_pool, s->base);
	*s = (struct heap_slot){0};
}

size_t app_platform_heap_bytes(int id)
{
	return slots[id].size;
}

#else /* POOL_SIZE > 0 */

int app_platform_heap_carve(int id, struct k_mem_domain *dom, size_t heap_size)
{
	ARG_UNUSED(id);
	ARG_UNUSED(dom);
	return (heap_size == 0U) ? 0 : -ENOTSUP;
}

void app_platform_heap_reset(int id)
{
	ARG_UNUSED(id);
}

void app_platform_heap_release(int id, struct k_mem_domain *dom)
{
	ARG_UNUSED(id);
	ARG_UNUSED(dom);
}

size_t app_platform_heap_bytes(int id)
{
	ARG_UNUSED(id);
	return 0U;
}

#endif /* POOL_SIZE > 0 */
