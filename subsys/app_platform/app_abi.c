/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Kernel services and the heap allocator exported to loadable application
 * modules. The kernel services take units that do not depend on the kernel
 * configuration.
 */

#include <zephyr/app_platform/app_abi.h>
#include <zephyr/app_platform/app_heap.h>
#include <zephyr/kernel.h>

int32_t app_abi_sleep_ms_v1(int32_t ms)
{
	return k_msleep(ms);
}

int32_t app_abi_sleep_us_v1(int32_t us)
{
	return k_usleep(us);
}

int64_t app_abi_uptime_get_v1(void)
{
	return k_uptime_get();
}

void app_abi_yield_v1(void)
{
	k_yield();
}

void *app_abi_malloc_v1(size_t size)
{
	return app_malloc(size);
}

void *app_abi_calloc_v1(size_t nmemb, size_t size)
{
	return app_calloc(nmemb, size);
}

void *app_abi_aligned_alloc_v1(size_t align, size_t size)
{
	return app_aligned_alloc(align, size);
}

void *app_abi_realloc_v1(void *ptr, size_t size)
{
	return app_realloc(ptr, size);
}

void app_abi_free_v1(void *ptr)
{
	app_free(ptr);
}
