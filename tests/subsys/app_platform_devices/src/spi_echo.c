/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* An emulated SPI device that clocks back the bytes it receives, so the test
 * sees which chip select and configuration a transfer used.
 */

#define DT_DRV_COMPAT vnd_spi_echo

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>
#include <string.h>

#include "spi_echo.h"

struct spi_echo_data {
	struct spi_config last;
	bool used;
};

static int spi_echo_io(const struct emul *target, const struct spi_config *config,
		       const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	struct spi_echo_data *data = target->data;
	const uint8_t *tx = NULL;
	size_t tx_len = 0;

	data->last = *config;
	data->used = true;
	if (tx_bufs != NULL && tx_bufs->count > 0) {
		tx = tx_bufs->buffers[0].buf;
		tx_len = tx_bufs->buffers[0].len;
	}
	if (rx_bufs != NULL && rx_bufs->count > 0) {
		uint8_t *rx = rx_bufs->buffers[0].buf;

		for (size_t i = 0; i < rx_bufs->buffers[0].len; i++) {
			rx[i] = (i < tx_len) ? tx[i] : 0U;
		}
	}
	return 0;
}

static const struct spi_emul_api spi_echo_api = {
	.io = spi_echo_io,
};

static int spi_echo_emul_init(const struct emul *target, const struct device *parent)
{
	ARG_UNUSED(target);
	ARG_UNUSED(parent);
	return 0;
}

static int spi_echo_dev_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define SPI_ECHO(n)                                                                        \
	static struct spi_echo_data spi_echo_data_##n;                                     \
	EMUL_DT_INST_DEFINE(n, spi_echo_emul_init, &spi_echo_data_##n, NULL, &spi_echo_api, \
			    NULL);                                                         \
	DEVICE_DT_INST_DEFINE(n, spi_echo_dev_init, NULL, NULL, NULL, POST_KERNEL,          \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

DT_INST_FOREACH_STATUS_OKAY(SPI_ECHO)

#define SPI_ECHO_REF(n) [DT_INST_REG_ADDR(n)] = &spi_echo_data_##n,

static struct spi_echo_data *const spi_echo_by_cs[] = {
	DT_INST_FOREACH_STATUS_OKAY(SPI_ECHO_REF)
};

const struct spi_config *spi_echo_last(int cs)
{
	if (cs < 0 || cs >= (int)ARRAY_SIZE(spi_echo_by_cs) || spi_echo_by_cs[cs] == NULL ||
	    !spi_echo_by_cs[cs]->used) {
		return NULL;
	}
	return &spi_echo_by_cs[cs]->last;
}
