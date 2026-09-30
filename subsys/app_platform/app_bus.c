/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Shared bus services: I2C, SPI and a secondary UART. Each is capability-gated
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

#include "app_grants.h"

#define BUS_XFER_MAX 64

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

int z_impl_app_svc_i2c_read(uint16_t addr, uint8_t *buf, size_t len)
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

int z_impl_app_svc_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
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

#if DT_HAS_CHOSEN(app_uart)
/* The port is shared, so the configuration an application installs is backed
 * by the one found at boot and restored on close or reclaim.
 */
static struct uart_config uart_boot_cfg;
static bool uart_boot_cfg_valid;
static bool uart_reconfigured;

static void uart_save_boot_cfg(void)
{
	if (uart_boot_cfg_valid) {
		return;
	}
	if (uart_config_get(uart_dev, &uart_boot_cfg) == 0) {
		uart_boot_cfg_valid = true;
	}
}

static void uart_restore_boot_cfg(int id)
{
	ARG_UNUSED(id);

	if (uart_reconfigured && uart_boot_cfg_valid) {
		(void)uart_configure(uart_dev, &uart_boot_cfg);
		uart_reconfigured = false;
	}
}
#endif

int z_impl_app_svc_uart_configure(uint32_t baud, uint8_t parity, uint8_t stop_bits,
				  uint8_t data_bits)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(app_uart)
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}

	struct uart_config cfg;

	if (baud == 0U) {
		return -EINVAL;
	}

	switch (parity) {
	case APP_UART_PARITY_NONE:
		cfg.parity = UART_CFG_PARITY_NONE;
		break;
	case APP_UART_PARITY_ODD:
		cfg.parity = UART_CFG_PARITY_ODD;
		break;
	case APP_UART_PARITY_EVEN:
		cfg.parity = UART_CFG_PARITY_EVEN;
		break;
	default:
		return -EINVAL;
	}

	switch (stop_bits) {
	case 1:
		cfg.stop_bits = UART_CFG_STOP_BITS_1;
		break;
	case 2:
		cfg.stop_bits = UART_CFG_STOP_BITS_2;
		break;
	default:
		return -EINVAL;
	}

	switch (data_bits) {
	case 5:
		cfg.data_bits = UART_CFG_DATA_BITS_5;
		break;
	case 6:
		cfg.data_bits = UART_CFG_DATA_BITS_6;
		break;
	case 7:
		cfg.data_bits = UART_CFG_DATA_BITS_7;
		break;
	case 8:
		cfg.data_bits = UART_CFG_DATA_BITS_8;
		break;
	case 9:
		cfg.data_bits = UART_CFG_DATA_BITS_9;
		break;
	default:
		return -EINVAL;
	}

	cfg.baudrate = baud;
	cfg.flow_ctrl = UART_CFG_FLOW_CTRL_NONE;

	uart_save_boot_cfg();

	int err = uart_configure(uart_dev, &cfg);

	if (err == 0) {
		uart_reconfigured = true;
	}
	return err;
#else
	ARG_UNUSED(baud);
	ARG_UNUSED(parity);
	ARG_UNUSED(stop_bits);
	ARG_UNUSED(data_bits);
	return -ENODEV;
#endif
}

int z_impl_app_svc_uart_close(void)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(app_uart)
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}
	uart_restore_boot_cfg(-1);
	return 0;
#else
	return -ENODEV;
#endif
}

int z_impl_app_svc_uart_write(const uint8_t *buf, size_t len)
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

int z_impl_app_svc_uart_read(uint8_t *buf, size_t len)
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

int z_impl_app_svc_i2c_write(uint16_t addr, const uint8_t *buf, size_t len)
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
	return i2c_write(i2c_dev, buf, len, addr);
#else
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

int z_impl_app_svc_i2c_write_read(uint16_t addr, const uint8_t *tx, size_t tx_len,
				  uint8_t *rx, size_t rx_len)
{
	if (!app_has_cap(APP_CAP_I2C)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || tx_len > BUS_XFER_MAX ||
	    rx_len > BUS_XFER_MAX) {
		return -EINVAL;
	}
	if (i2c_grant[id] == 0U || i2c_grant[id] != addr) {
		return -EACCES;
	}
#if DT_HAS_ALIAS(app_i2c)
	if (!device_is_ready(i2c_dev)) {
		return -ENODEV;
	}
	return i2c_write_read(i2c_dev, addr, tx, tx_len, rx, rx_len);
#else
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_i2c_read(uint16_t addr, uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_app_svc_i2c_read(addr, buf, len);
}
#include <zephyr/syscalls/app_svc_i2c_read_mrsh.c>

static inline int z_vrfy_app_svc_i2c_write(uint16_t addr, const uint8_t *buf,
					   size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_app_svc_i2c_write(addr, buf, len);
}
#include <zephyr/syscalls/app_svc_i2c_write_mrsh.c>

static inline int z_vrfy_app_svc_i2c_write_read(uint16_t addr, const uint8_t *tx,
						size_t tx_len, uint8_t *rx,
						size_t rx_len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(tx, tx_len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(rx, rx_len));
	return z_impl_app_svc_i2c_write_read(addr, tx, tx_len, rx, rx_len);
}
#include <zephyr/syscalls/app_svc_i2c_write_read_mrsh.c>

static inline int z_vrfy_app_svc_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(tx, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(rx, len));
	return z_impl_app_svc_spi_xfer(tx, rx, len);
}
#include <zephyr/syscalls/app_svc_spi_xfer_mrsh.c>

static inline int z_vrfy_app_svc_uart_write(const uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_app_svc_uart_write(buf, len);
}
#include <zephyr/syscalls/app_svc_uart_write_mrsh.c>

static inline int z_vrfy_app_svc_uart_read(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_app_svc_uart_read(buf, len);
}
#include <zephyr/syscalls/app_svc_uart_read_mrsh.c>

static inline int z_vrfy_app_svc_uart_configure(uint32_t baud, uint8_t parity,
						uint8_t stop_bits, uint8_t data_bits)
{
	return z_impl_app_svc_uart_configure(baud, parity, stop_bits, data_bits);
}
#include <zephyr/syscalls/app_svc_uart_configure_mrsh.c>

static inline int z_vrfy_app_svc_uart_close(void)
{
	return z_impl_app_svc_uart_close();
}
#include <zephyr/syscalls/app_svc_uart_close_mrsh.c>
#endif

#if DT_HAS_CHOSEN(app_uart)
static int uart_service_init(void)
{
	return app_platform_on_reclaim(uart_restore_boot_cfg);
}
SYS_INIT(uart_service_init, APPLICATION, 99);
#endif
