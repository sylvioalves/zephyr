/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The shared memory-to-memory DMA service. The platform owns the DMA channel and
 * a pair of DMA-capable buffers; an application holding APP_CAP_DMA hands in
 * bytes, the platform copies them through the DMA engine and hands the result
 * back. The application never touches the channel or the cache. Lock-serialized,
 * since the buffers and channel are shared.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/cache.h>
#include <string.h>
#include <errno.h>
#include "app_grants.h"

#define DMA_MAX  256
#define DMA_CHAN 0U

#if DT_NODE_EXISTS(DT_NODELABEL(dma))
static const struct device *const dma_dev = DEVICE_DT_GET(DT_NODELABEL(dma));
static uint8_t dma_src[DMA_MAX] __aligned(64);
static uint8_t dma_dst[DMA_MAX] __aligned(64);
static SVC_LOCK_DEFINE(dma_lock);
static K_SEM_DEFINE(dma_done_sem, 0, 1);

static void dma_done(const struct device *d, void *arg, uint32_t chan, int status)
{
	ARG_UNUSED(d);
	ARG_UNUSED(arg);
	ARG_UNUSED(chan);
	ARG_UNUSED(status);
	k_sem_give(&dma_done_sem);
}
#endif

int z_impl_app_svc_dma_copy(const uint8_t *src, uint8_t *dst, size_t len)
{
	if (!app_has_cap(APP_CAP_DMA)) {
		return -EPERM;
	}
	if (len > DMA_MAX) {
		return -EINVAL;
	}
#if DT_NODE_EXISTS(DT_NODELABEL(dma))
	uint8_t tmp[DMA_MAX];

	if (!device_is_ready(dma_dev)) {
		return -ENODEV;
	}

	memcpy(tmp, src, len);
	if (app_service_lock(&dma_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}

	memcpy(dma_src, tmp, len);
	sys_cache_data_flush_range(dma_src, len);
	sys_cache_data_invd_range(dma_dst, len);

	struct dma_block_config blk = {
		.source_address = (uintptr_t)dma_src,
		.dest_address = (uintptr_t)dma_dst,
		.block_size = len,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1,
		.dest_data_size = 1,
		.block_count = 1,
		.head_block = &blk,
		.dma_callback = dma_done,
	};

	int rc = dma_config(dma_dev, DMA_CHAN, &cfg);

	if (rc == 0) {
		k_sem_reset(&dma_done_sem);
		rc = dma_start(dma_dev, DMA_CHAN);
		if (rc == 0 && k_sem_take(&dma_done_sem, K_MSEC(100)) == 0) {
			sys_cache_data_invd_range(dma_dst, len);
			memcpy(tmp, dma_dst, len);
		} else {
			rc = -EIO;
		}
	}
	app_service_unlock(&dma_lock);
	if (rc == 0) {
		memcpy(dst, tmp, len);
	}
	return rc;
#else
	ARG_UNUSED(src);
	ARG_UNUSED(dst);
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_dma_copy(const uint8_t *src, uint8_t *dst, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(src, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(dst, len));
	return z_impl_app_svc_dma_copy(src, dst, len);
}
#include <zephyr/syscalls/app_svc_dma_copy_mrsh.c>
#endif
