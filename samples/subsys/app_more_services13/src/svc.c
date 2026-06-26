/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/cache.h>
#include <string.h>
#include <errno.h>

#define DMA_MAX  256
#define DMA_CHAN 0U

/* The platform owns the GDMA controller and the DMA-capable staging buffers. */
static const struct device *const dma = DEVICE_DT_GET(DT_NODELABEL(dma));
static const uint32_t dma_chan = DMA_CHAN;

/* Cache-line aligned so the DMA and CPU views stay coherent on flush/invalidate. */
static uint8_t src_buf[DMA_MAX] __aligned(64);
static uint8_t dst_buf[DMA_MAX] __aligned(64);

static struct k_sem done_sem;

static void dma_done(const struct device *d, void *arg, uint32_t chan, int status)
{
	ARG_UNUSED(d); ARG_UNUSED(arg); ARG_UNUSED(chan); ARG_UNUSED(status);
	k_sem_give(&done_sem);
}

int svc_dma_install(void)
{
	if (!device_is_ready(dma)) {
		return -ENODEV;
	}
	k_sem_init(&done_sem, 0, 1);
	return 0;
}

int z_impl_svc_dma_copy(const uint8_t *src, uint8_t *dst, size_t len)
{
	if (!device_is_ready(dma) || len == 0U || len > DMA_MAX) {
		return -EINVAL;
	}

	memcpy(src_buf, src, len);
	sys_cache_data_flush_range(src_buf, len);

	struct dma_block_config blk = {
		.source_address = (uint32_t)src_buf,
		.dest_address = (uint32_t)dst_buf,
		.block_size = len,
	};
	struct dma_config cfg = {
		.channel_direction = MEMORY_TO_MEMORY,
		.source_data_size = 1U,
		.dest_data_size = 1U,
		.source_burst_length = 1U,
		.dest_burst_length = 1U,
		.block_count = 1U,
		.head_block = &blk,
		.dma_callback = dma_done,
		.complete_callback_en = 0U,
	};

	int rc = dma_config(dma, dma_chan, &cfg);

	if (rc != 0) {
		return rc;
	}

	k_sem_reset(&done_sem);
	rc = dma_start(dma, dma_chan);
	if (rc != 0) {
		return rc;
	}

	if (k_sem_take(&done_sem, K_MSEC(100)) != 0) {
		return -ETIMEDOUT;
	}

	sys_cache_data_invd_range(dst_buf, len);
	memcpy(dst, dst_buf, len);
	return (int)len;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_dma_copy(const uint8_t *src, uint8_t *dst, size_t len)
{
	if (len == 0U || len > DMA_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(src, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(dst, len));
	return z_impl_svc_dma_copy(src, dst, len);
}
#include <zephyr/syscalls/svc_dma_copy_mrsh.c>
#endif
