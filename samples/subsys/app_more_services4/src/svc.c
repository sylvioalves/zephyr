/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <errno.h>

#define XFER_MAX 32

/* The platform owns the SPI controller (bus 2 on the devkit). */
static const struct device *const spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));

/* One spi_config per chip-select: same bus, different CS gpio. */
static struct spi_config spi_cfg[] = {
	{
		.frequency = 1000000,
		.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
		.cs = SPI_CS_CONTROL_INIT(DT_NODELABEL(spi_target0)),
	},
	{
		.frequency = 1000000,
		.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
		.cs = SPI_CS_CONTROL_INIT(DT_NODELABEL(spi_target1)),
	},
};
#define CS_COUNT ARRAY_SIZE(spi_cfg)

/* Per-chip-select grant: bit N set means the app may talk to target N. */
static uint32_t spi_grant;

void svc_spi_grant(uint32_t cs_mask)
{
	spi_grant = cs_mask;
}

int z_impl_svc_spi_xfer(int cs, const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (cs < 0 || cs >= (int)CS_COUNT || !device_is_ready(spi_dev) ||
	    len > XFER_MAX) {
		return -EINVAL;
	}
	struct spi_buf txb = { .buf = (void *)tx, .len = len };
	struct spi_buf rxb = { .buf = rx, .len = len };
	struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };

	int rc = spi_transceive(spi_dev, &spi_cfg[cs], &txs, &rxs);

	return (rc == 0) ? (int)len : rc;
}

int z_impl_svc_device_id(uint8_t *buf, size_t len)
{
	return (int)hwinfo_get_device_id(buf, len);
}

int z_impl_svc_reset_cause(uint32_t *cause)
{
	return hwinfo_get_reset_cause(cause);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_spi_xfer(int cs, const uint8_t *tx, uint8_t *rx, size_t len)
{
	/* Reject any chip-select the app was not granted. */
	if (cs < 0 || cs >= (int)CS_COUNT || (spi_grant & BIT(cs)) == 0U) {
		return -EPERM;
	}
	if (len > XFER_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(tx, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(rx, len));
	return z_impl_svc_spi_xfer(cs, tx, rx, len);
}
#include <zephyr/syscalls/svc_spi_xfer_mrsh.c>

static inline int z_vrfy_svc_device_id(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_device_id(buf, len);
}
#include <zephyr/syscalls/svc_device_id_mrsh.c>

static inline int z_vrfy_svc_reset_cause(uint32_t *cause)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(cause, sizeof(*cause)));
	return z_impl_svc_reset_cause(cause);
}
#include <zephyr/syscalls/svc_reset_cause_mrsh.c>
#endif
