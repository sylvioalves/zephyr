/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared bus services: I2C, SPI and a secondary UART. Each is capability-gated
 * and scoped: I2C and SPI by a per-application device grant, UART by holding the
 * capability. The bus controllers live in the platform; an application transfers
 * bytes through copied buffers and never holds a bus handle. Each service builds
 * to a stub when its devicetree node is absent.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <errno.h>

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS
#define BUS_XFER_MAX 64

static inline bool app_has_cap(uint32_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* ---- I2C ---- */

#if DT_HAS_ALIAS(app_i2c)
static const struct device *const i2c_dev = DEVICE_DT_GET(DT_ALIAS(app_i2c));
#endif

/* Per-application grant of a single allowed 7-bit address (0 = none). */
static uint16_t i2c_grant[MAX_APPS];

void app_platform_grant_i2c(int id, uint16_t addr)
{
	if (id >= 0 && id < MAX_APPS) {
		i2c_grant[id] = addr;
	}
}

int z_impl_svc_i2c_read(uint16_t addr, uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_I2C)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || len > BUS_XFER_MAX) {
		return -EINVAL;
	}
	if (i2c_grant[id] == 0U || i2c_grant[id] != addr) {
		return -EACCES;
	}
#if DT_HAS_ALIAS(app_i2c)
	if (!device_is_ready(i2c_dev)) {
		return -ENODEV;
	}
	return i2c_read(i2c_dev, buf, len, addr);
#else
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

/* ---- SPI ---- */

#if DT_HAS_ALIAS(app_spi)
static const struct device *const spi_dev = DEVICE_DT_GET(DT_ALIAS(app_spi));
static struct spi_config spi_cfg = {
	.frequency = 1000000U,
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
};
#endif

static bool spi_grant[MAX_APPS];

/* Drop the bus grants of a slot being handed to a new application. */
void app_platform_clear_bus_grants(int id)
{
	if (id >= 0 && id < MAX_APPS) {
		i2c_grant[id] = 0U;
		spi_grant[id] = false;
	}
}

void app_platform_grant_spi(int id, bool allow)
{
	if (id >= 0 && id < MAX_APPS) {
		spi_grant[id] = allow;
	}
}

int z_impl_svc_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (!app_has_cap(APP_CAP_SPI)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || len > BUS_XFER_MAX) {
		return -EINVAL;
	}
	if (!spi_grant[id]) {
		return -EACCES;
	}
#if DT_HAS_ALIAS(app_spi)
	if (!device_is_ready(spi_dev)) {
		return -ENODEV;
	}
	struct spi_buf txb = { .buf = (void *)tx, .len = len };
	struct spi_buf rxb = { .buf = rx, .len = len };
	struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };

	return spi_transceive(spi_dev, &spi_cfg, &txs, &rxs);
#else
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);
	return -ENODEV;
#endif
}

/* ---- UART (secondary, polled) ---- */

#if DT_HAS_CHOSEN(app_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(app_uart));
#endif

int z_impl_svc_uart_write(const uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}
	if (len > BUS_XFER_MAX) {
		return -EINVAL;
	}
#if DT_HAS_CHOSEN(app_uart)
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, buf[i]);
	}
	return (int)len;
#else
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

int z_impl_svc_uart_read(uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}
	if (len > BUS_XFER_MAX) {
		return -EINVAL;
	}
#if DT_HAS_CHOSEN(app_uart)
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}
	size_t n = 0;

	while (n < len && uart_poll_in(uart_dev, &buf[n]) == 0) {
		n++;
	}
	return (int)n;
#else
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_i2c_read(uint16_t addr, uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_i2c_read(addr, buf, len);
}
#include <zephyr/syscalls/svc_i2c_read_mrsh.c>

static inline int z_vrfy_svc_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(tx, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(rx, len));
	return z_impl_svc_spi_xfer(tx, rx, len);
}
#include <zephyr/syscalls/svc_spi_xfer_mrsh.c>

static inline int z_vrfy_svc_uart_write(const uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_uart_write(buf, len);
}
#include <zephyr/syscalls/svc_uart_write_mrsh.c>

static inline int z_vrfy_svc_uart_read(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_uart_read(buf, len);
}
#include <zephyr/syscalls/svc_uart_read_mrsh.c>
#endif
