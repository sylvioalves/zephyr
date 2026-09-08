/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The external-RAM (PSRAM) mapping service. The platform allocates a region of
 * external RAM and adds it as a partition to the calling application's own
 * memory domain, so the application reads and writes it directly at full speed -
 * no per-access syscall, unlike a brokered copy. The region is power-of-two
 * sized and naturally aligned for the memory-protection hardware, and is freed
 * when the application is reclaimed. Each application may hold one region.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <zephyr/init.h>
#include <errno.h>

#define MAX_APPS  CONFIG_APP_PLATFORM_MAX_APPS
#define PSRAM_MIN 1024U
#define PSRAM_MAX ((size_t)CONFIG_APP_PLATFORM_PSRAM_MAX_BYTES)

/* Bounded wait for the shared lock; -EBUSY rather than parking the caller. */
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* One mapped region per application group. */
struct psram_region {
	void                   *base;
	size_t                  size;
	struct k_mem_partition  part;
	bool                    used;
};
static struct psram_region regions[MAX_APPS];
static K_MUTEX_DEFINE(psram_lock);

/* Smallest power of two at or above @p v, for the NAPOT-style alignment the
 * memory partition needs. The caller bounds v by PSRAM_MAX, so the shift
 * cannot walk p into an overflow.
 */
static size_t round_pow2(size_t v)
{
	size_t p = PSRAM_MIN;

	while (p < v && p < PSRAM_MAX) {
		p <<= 1;
	}
	return p;
}

static void psram_release(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}

	k_mutex_lock(&psram_lock, K_FOREVER);
	if (regions[id].used) {
		struct k_mem_domain *dom = app_platform_domain(id);

		if (dom != NULL) {
			(void)k_mem_domain_remove_partition(dom, &regions[id].part);
		}
		shared_multi_heap_free(regions[id].base);
		regions[id] = (struct psram_region){0};
	}
	k_mutex_unlock(&psram_lock);
}

int z_impl_svc_psram_map(size_t size, uintptr_t *out_addr, size_t *out_size)
{
	if (!app_has_cap(APP_CAP_PSRAM)) {
		return -EPERM;
	}

	int id = app_platform_id_current();
	struct k_mem_domain *dom = app_platform_domain(id);

	if (id < 0 || id >= MAX_APPS || dom == NULL) {
		return -EINVAL;
	}
	/* Bound the request before any rounding, so a wild size cannot spin the
	 * search for a power of two while the shared lock is held.
	 */
	if (size == 0U || size > PSRAM_MAX) {
		return -EINVAL;
	}

	if (k_mutex_lock(&psram_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	if (regions[id].used) {
		k_mutex_unlock(&psram_lock);
		return -EEXIST;
	}

	size_t want = round_pow2(size);
	void *base = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL,
						     want, want);

	if (base == NULL) {
		k_mutex_unlock(&psram_lock);
		return -ENOMEM;
	}

	regions[id].base = base;
	regions[id].size = want;
	regions[id].part = (struct k_mem_partition){
		.start = (uintptr_t)base,
		.size = want,
		.attr = K_MEM_PARTITION_P_RW_U_RW,
	};

	int rc = k_mem_domain_add_partition(dom, &regions[id].part);

	if (rc != 0) {
		shared_multi_heap_free(base);
		regions[id] = (struct psram_region){0};
		k_mutex_unlock(&psram_lock);
		return rc;
	}
	regions[id].used = true;
	k_mutex_unlock(&psram_lock);

	*out_addr = (uintptr_t)base;
	*out_size = want;
	return 0;
}

int z_impl_svc_psram_unmap(void)
{
	if (!app_has_cap(APP_CAP_PSRAM)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || !regions[id].used) {
		return -EINVAL;
	}
	psram_release(id);
	return 0;
}

static int psram_service_init(void)
{
	return app_platform_on_reclaim(psram_release);
}
SYS_INIT(psram_service_init, APPLICATION, 99);

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_psram_map(size_t size, uintptr_t *out_addr,
				       size_t *out_size)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out_addr, sizeof(*out_addr)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out_size, sizeof(*out_size)));
	return z_impl_svc_psram_map(size, out_addr, out_size);
}
#include <zephyr/syscalls/svc_psram_map_mrsh.c>

static inline int z_vrfy_svc_psram_unmap(void)
{
	return z_impl_svc_psram_unmap();
}
#include <zephyr/syscalls/svc_psram_unmap_mrsh.c>
#endif
