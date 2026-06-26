/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <errno.h>

#define BLOCK_SIZE  1024
#define NUM_BLOCKS  4

/* The platform owns the I2S controller and the transmit memory slab. */
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_ALIAS(i2s_tx));
K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLOCK_SIZE, NUM_BLOCKS, 4);

static int queued;
static bool started;

int svc_audio_install(void)
{
	if (!device_is_ready(i2s_dev)) {
		return -ENODEV;
	}

	struct i2s_config cfg = {
		.word_size = 16U,
		.channels = 2U,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.frame_clk_freq = 44100,
		.block_size = BLOCK_SIZE,
		.timeout = 2000,
		.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
		.mem_slab = &tx_slab,
	};

	return i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
}

int z_impl_svc_audio_play(const uint8_t *pcm, size_t len)
{
	void *block;
	int rc;

	if (!device_is_ready(i2s_dev) || len == 0U || len > BLOCK_SIZE) {
		return -EINVAL;
	}

	rc = k_mem_slab_alloc(&tx_slab, &block, K_MSEC(200));
	if (rc != 0) {
		return rc;
	}

	memcpy(block, pcm, len);
	if (len < BLOCK_SIZE) {
		memset((uint8_t *)block + len, 0, BLOCK_SIZE - len);
	}

	rc = i2s_write(i2s_dev, block, BLOCK_SIZE);
	if (rc != 0) {
		k_mem_slab_free(&tx_slab, block);
		return rc;
	}

	/* Build a small runway before starting so the DMA does not underrun. */
	queued++;
	if (!started && queued >= 2) {
		rc = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		if (rc != 0) {
			return rc;
		}
		started = true;
	}

	return (int)len;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_audio_play(const uint8_t *pcm, size_t len)
{
	if (len == 0U || len > BLOCK_SIZE) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(pcm, len));
	return z_impl_svc_audio_play(pcm, len);
}
#include <zephyr/syscalls/svc_audio_play_mrsh.c>
#endif
