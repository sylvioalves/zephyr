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
#include <zephyr/dt-bindings/i2c/i2c.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#include "app_events.h"
#include "app_grants.h"

LOG_MODULE_DECLARE(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

#define BUS_XFER_MAX    CONFIG_APP_PLATFORM_BUS_XFER_MAX
#define I2C_ADDRS       CONFIG_APP_PLATFORM_I2C_ADDRS
#define I2C_ERROR_LIMIT CONFIG_APP_PLATFORM_I2C_ERROR_LIMIT

/* ---- I2C ---- */

#if DT_HAS_ALIAS(app_i2c)
static const struct device *const i2c_dev = DEVICE_DT_GET(DT_ALIAS(app_i2c));
#endif

/* Per-application 7-bit addresses (0 = free entry). An address is granted to
 * at most one application at a time.
 */
static uint16_t i2c_grant[MAX_APPS][I2C_ADDRS];
/* Consecutive failed transfers per application. */
static uint8_t i2c_errors[MAX_APPS];
static struct k_spinlock i2c_grant_lock;
/* Bus speed each application asked for, APP_I2C_SPEED_*; 0 is the devicetree
 * speed. It is applied before each of the application's transfers, under the
 * bus lock, so it never applies to another application's transfer.
 */
static uint8_t i2c_speed[MAX_APPS];
static SVC_LOCK_DEFINE(i2c_bus_lock);

#if DT_HAS_ALIAS(app_i2c)
static uint8_t i2c_cur_speed;

#define I2C_DT_HZ DT_PROP_OR(DT_ALIAS(app_i2c), clock_frequency, I2C_BITRATE_STANDARD)
#define I2C_DT_SPEED                                                                       \
	((I2C_DT_HZ >= I2C_BITRATE_FAST_PLUS) ? I2C_SPEED_FAST_PLUS                         \
	 : (I2C_DT_HZ >= I2C_BITRATE_FAST)    ? I2C_SPEED_FAST                              \
					      : I2C_SPEED_STANDARD)

/* Run one transfer at the calling application's speed. */
static int i2c_run(int id, struct i2c_msg *msgs, uint8_t num, uint16_t addr)
{
	int rc;

	if (app_service_lock(&i2c_bus_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	if (i2c_speed[id] != i2c_cur_speed) {
		uint32_t speed = (i2c_speed[id] != 0U) ? i2c_speed[id] : I2C_DT_SPEED;

		rc = i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(speed));
		if (rc != 0) {
			(void)app_service_unlock(&i2c_bus_lock);
			return rc;
		}
		i2c_cur_speed = i2c_speed[id];
	}
	rc = i2c_transfer(i2c_dev, msgs, num, addr);
	(void)app_service_unlock(&i2c_bus_lock);
	return rc;
}
#endif

int app_platform_grant_i2c(int id, uint16_t addr)
{
	if (id < 0 || id >= MAX_APPS || addr == 0U || addr > 0x7fU) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&i2c_grant_lock);

	for (int a = 0; a < MAX_APPS; a++) {
		for (int i = 0; i < I2C_ADDRS; i++) {
			if (i2c_grant[a][i] == addr) {
				k_spin_unlock(&i2c_grant_lock, key);
				return (a == id) ? 0 : -EBUSY;
			}
		}
	}
	for (int i = 0; i < I2C_ADDRS; i++) {
		if (i2c_grant[id][i] == 0U) {
			i2c_grant[id][i] = addr;
			k_spin_unlock(&i2c_grant_lock, key);
			return 0;
		}
	}
	k_spin_unlock(&i2c_grant_lock, key);
	return -ENOSPC;
}

static void i2c_revoke_all(int id)
{
	k_spinlock_key_t key = k_spin_lock(&i2c_grant_lock);

	for (int i = 0; i < I2C_ADDRS; i++) {
		i2c_grant[id][i] = 0U;
	}
	i2c_errors[id] = 0U;
	k_spin_unlock(&i2c_grant_lock, key);
}

/* Admit an I2C call: capability, transfer bounds and address grant. */
static int i2c_admit(uint16_t addr, size_t len_a, size_t len_b, int *id_out)
{
	if (!app_has_cap(APP_CAP_I2C)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || len_a > BUS_XFER_MAX || len_b > BUS_XFER_MAX) {
		return -EINVAL;
	}
	if (addr == 0U) {
		return -EACCES;
	}

	bool granted = false;
	k_spinlock_key_t key = k_spin_lock(&i2c_grant_lock);

	for (int i = 0; i < I2C_ADDRS; i++) {
		if (i2c_grant[id][i] == addr) {
			granted = true;
			break;
		}
	}
	k_spin_unlock(&i2c_grant_lock, key);
	if (!granted) {
		return -EACCES;
	}
#if DT_HAS_ALIAS(app_i2c)
	if (!device_is_ready(i2c_dev)) {
		return -ENODEV;
	}
#endif
	*id_out = id;
	return 0;
}

#if DT_HAS_ALIAS(app_i2c)
/* Count failed transfers. An application whose transfers keep failing has its
 * grants revoked and the bus recovered, so a device it holds in a bad state, or
 * a line it keeps stuck, stops costing every other application the bus.
 */
static int i2c_account(int id, int rc)
{
	if (rc == 0) {
		i2c_errors[id] = 0U;
		return 0;
	}
	if (I2C_ERROR_LIMIT > 0 && ++i2c_errors[id] >= I2C_ERROR_LIMIT) {
		LOG_WRN("app %d: %d failed i2c transfers, grants revoked", id, I2C_ERROR_LIMIT);
		i2c_revoke_all(id);
		(void)i2c_recover_bus(i2c_dev);
	}
	return rc;
}
#endif

int z_impl_app_svc_i2c_read(uint16_t addr, uint8_t *buf, size_t len)
{
	int id;
	int rc = i2c_admit(addr, len, 0, &id);

	if (rc != 0) {
		return rc;
	}
#if DT_HAS_ALIAS(app_i2c)
	struct i2c_msg msg = { .buf = buf, .len = len, .flags = I2C_MSG_READ | I2C_MSG_STOP };

	return i2c_account(id, i2c_run(id, &msg, 1, addr));
#else
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

int z_impl_app_svc_i2c_write(uint16_t addr, const uint8_t *buf, size_t len)
{
	int id;
	int rc = i2c_admit(addr, len, 0, &id);

	if (rc != 0) {
		return rc;
	}
#if DT_HAS_ALIAS(app_i2c)
	struct i2c_msg msg = { .buf = (uint8_t *)buf, .len = len,
			       .flags = I2C_MSG_WRITE | I2C_MSG_STOP };

	return i2c_account(id, i2c_run(id, &msg, 1, addr));
#else
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

int z_impl_app_svc_i2c_write_read(uint16_t addr, const uint8_t *tx, size_t tx_len,
				  uint8_t *rx, size_t rx_len)
{
	int id;
	int rc = i2c_admit(addr, tx_len, rx_len, &id);

	if (rc != 0) {
		return rc;
	}
#if DT_HAS_ALIAS(app_i2c)
	struct i2c_msg msgs[2] = {
		{ .buf = (uint8_t *)tx, .len = tx_len, .flags = I2C_MSG_WRITE },
		{ .buf = rx, .len = rx_len,
		  .flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP },
	};

	return i2c_account(id, i2c_run(id, msgs, 2, addr));
#else
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);
	return -ENODEV;
#endif
}

int z_impl_app_svc_i2c_transfer(uint16_t addr, const struct app_i2c_msg *msgs, size_t num,
				uint8_t *buf, size_t buf_len)
{
	int id;
	int rc = i2c_admit(addr, buf_len, 0, &id);

	if (rc != 0) {
		return rc;
	}
	if (num == 0U || num > APP_I2C_MSGS_MAX) {
		return -EINVAL;
	}
#if DT_HAS_ALIAS(app_i2c)
	struct i2c_msg m[APP_I2C_MSGS_MAX];
	size_t off = 0U;

	for (size_t i = 0; i < num; i++) {
		uint8_t f = msgs[i].flags;

		if ((f & ~(APP_I2C_MSG_READ | APP_I2C_MSG_STOP | APP_I2C_MSG_RESTART)) != 0U ||
		    msgs[i].len > buf_len - off) {
			return -EINVAL;
		}
		m[i].buf = &buf[off];
		m[i].len = msgs[i].len;
		m[i].flags = ((f & APP_I2C_MSG_READ) != 0U) ? I2C_MSG_READ : I2C_MSG_WRITE;
		if ((f & APP_I2C_MSG_STOP) != 0U || i == num - 1U) {
			m[i].flags |= I2C_MSG_STOP;
		}
		if ((f & APP_I2C_MSG_RESTART) != 0U) {
			m[i].flags |= I2C_MSG_RESTART;
		}
		off += msgs[i].len;
	}
	return i2c_account(id, i2c_run(id, m, (uint8_t)num, addr));
#else
	ARG_UNUSED(msgs);
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

int z_impl_app_svc_i2c_configure(uint8_t speed)
{
	if (!app_has_cap(APP_CAP_I2C)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || speed > APP_I2C_SPEED_FAST_PLUS) {
		return -EINVAL;
	}
	i2c_speed[id] = speed;
	return 0;
}

/* ---- SPI ---- */

#define SPI_CS_COUNT 32

#if DT_HAS_ALIAS(app_spi)
#define SPI_NODE DT_ALIAS(app_spi)
static const struct device *const spi_dev = DEVICE_DT_GET(SPI_NODE);

#if DT_NODE_HAS_PROP(SPI_NODE, cs_gpios)
#define SPI_CS_GPIO(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),
static const struct gpio_dt_spec spi_cs_gpios[] = {
	DT_FOREACH_PROP_ELEM(SPI_NODE, cs_gpios, SPI_CS_GPIO)
};
#endif

/* Each application transfers with its own configuration, so one application's
 * mode, clock or chip select never applies to another's transfer.
 */
static struct spi_config spi_cfg[MAX_APPS];
#endif

static bool spi_grant[MAX_APPS];
/* Bit N set: this application holds chip select N. A chip select is held by
 * one application at a time.
 */
static uint32_t spi_cs_grant[MAX_APPS];
static struct k_spinlock spi_grant_lock;

static void spi_reset_cfg(int id)
{
#if DT_HAS_ALIAS(app_spi)
	spi_cfg[id] = (struct spi_config){
		.frequency = 1000000U,
		.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
	};
#else
	ARG_UNUSED(id);
#endif
}

/* Drop the bus grants of a slot being handed to a new application. */
void app_platform_clear_bus_grants(int id)
{
	if (id >= 0 && id < MAX_APPS) {
		i2c_revoke_all(id);
		i2c_speed[id] = 0U;
		spi_grant[id] = false;
		spi_cs_grant[id] = 0U;
		spi_reset_cfg(id);
	}
}

void app_platform_grant_spi(int id, bool allow)
{
	if (id >= 0 && id < MAX_APPS) {
		spi_grant[id] = allow;
	}
}

int app_platform_grant_spi_cs(int id, uint8_t cs)
{
	if (id < 0 || id >= MAX_APPS || cs >= SPI_CS_COUNT) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&spi_grant_lock);

	for (int a = 0; a < MAX_APPS; a++) {
		if (a != id && (spi_cs_grant[a] & BIT(cs)) != 0U) {
			k_spin_unlock(&spi_grant_lock, key);
			return -EBUSY;
		}
	}
	spi_cs_grant[id] |= BIT(cs);
	spi_grant[id] = true;
	k_spin_unlock(&spi_grant_lock, key);
	return 0;
}

static int spi_admit(size_t len_a, size_t len_b, int *id_out)
{
	if (!app_has_cap(APP_CAP_SPI)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || len_a > BUS_XFER_MAX || len_b > BUS_XFER_MAX) {
		return -EINVAL;
	}
	if (!spi_grant[id]) {
		return -EACCES;
	}
#if DT_HAS_ALIAS(app_spi)
	if (!device_is_ready(spi_dev)) {
		return -ENODEV;
	}
	if (spi_cfg[id].frequency == 0U) {
		spi_reset_cfg(id);
	}
#endif
	*id_out = id;
	return 0;
}

int z_impl_app_svc_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	return z_impl_app_svc_spi_transceive(tx, len, rx, len);
}

int z_impl_app_svc_spi_transceive(const uint8_t *tx, size_t tx_len, uint8_t *rx,
				  size_t rx_len)
{
	int id;
	int rc = spi_admit(tx_len, rx_len, &id);

	if (rc != 0) {
		return rc;
	}
	if ((tx == NULL && tx_len != 0U) || (rx == NULL && rx_len != 0U) ||
	    tx_len + rx_len == 0U) {
		return -EINVAL;
	}
#if DT_HAS_ALIAS(app_spi)
	struct spi_buf txb = { .buf = (void *)tx, .len = tx_len };
	struct spi_buf rxb = { .buf = rx, .len = rx_len };
	struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };

	return spi_transceive(spi_dev, &spi_cfg[id], (tx_len != 0U) ? &txs : NULL,
			      (rx_len != 0U) ? &rxs : NULL);
#else
	return -ENODEV;
#endif
}

int z_impl_app_svc_spi_configure(uint32_t freq_hz, uint8_t flags, uint8_t word_bits,
				 uint8_t cs)
{
	int id;
	int rc = spi_admit(0, 0, &id);

	if (rc != 0) {
		return rc;
	}
	if (freq_hz == 0U || (word_bits != 8U && word_bits != 16U) ||
	    (flags & ~(APP_SPI_CPOL | APP_SPI_CPHA | APP_SPI_LSB_FIRST)) != 0U) {
		return -EINVAL;
	}
	if (cs != APP_SPI_CS_NONE && (cs >= SPI_CS_COUNT || (spi_cs_grant[id] & BIT(cs)) == 0U)) {
		return -EACCES;
	}
#if DT_HAS_ALIAS(app_spi)
	struct spi_config cfg = {
		.frequency = freq_hz,
		.operation = SPI_WORD_SET(word_bits) | SPI_OP_MODE_CONTROLLER,
	};

	if ((flags & APP_SPI_CPOL) != 0U) {
		cfg.operation |= SPI_MODE_CPOL;
	}
	if ((flags & APP_SPI_CPHA) != 0U) {
		cfg.operation |= SPI_MODE_CPHA;
	}
	cfg.operation |= ((flags & APP_SPI_LSB_FIRST) != 0U) ? SPI_TRANSFER_LSB : SPI_TRANSFER_MSB;
	if (cs != APP_SPI_CS_NONE) {
#if DT_NODE_HAS_PROP(SPI_NODE, cs_gpios)
		if (cs >= ARRAY_SIZE(spi_cs_gpios)) {
			return -EINVAL;
		}
		cfg.cs.gpio = spi_cs_gpios[cs];
#endif
		cfg.peripheral = cs;
	}
	spi_cfg[id] = cfg;
	return 0;
#else
	return -ENODEV;
#endif
}

/* ---- UART (secondary) ---- */

#if DT_HAS_CHOSEN(app_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(app_uart));
static SVC_LOCK_DEFINE(uart_lock);
/* Applications that used the port, told when bytes arrive. */
static atomic_t uart_users;
static atomic_t uart_errs;
#endif

#if DT_HAS_CHOSEN(app_uart) && defined(CONFIG_UART_INTERRUPT_DRIVEN)
/* Once an application uses the port, received bytes are taken by the interrupt
 * into a buffer, so none are lost between two reads of the application.
 */
RING_BUF_DECLARE(uart_rx_rb, CONFIG_APP_PLATFORM_UART_RX_BUF);
static struct k_spinlock uart_rx_lock;
static bool uart_irq_on;

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uint8_t buf[16];

	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) > 0) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}

		k_spinlock_key_t key = k_spin_lock(&uart_rx_lock);
		uint32_t put = ring_buf_put(&uart_rx_rb, buf, (uint32_t)n);

		k_spin_unlock(&uart_rx_lock, key);
		if (put < (uint32_t)n) {
			atomic_or(&uart_errs, APP_UART_ERR_RX_DROPPED);
		}
		for (int id = 0; id < MAX_APPS; id++) {
			if (atomic_test_bit(&uart_users, id)) {
				app_events_notify(id, APP_EVENT_UART_RX, 0U);
			}
		}
	}
}
#endif

#if DT_HAS_CHOSEN(app_uart)
/* Register the calling application as a user and start interrupt reception. */
static void uart_join(void)
{
	int id = app_platform_id_current();

	if (id >= 0 && id < MAX_APPS) {
		atomic_set_bit(&uart_users, id);
	}
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	if (!uart_irq_on && app_service_lock(&uart_lock, SVC_LOCK_TIMEOUT) == 0) {
		if (!uart_irq_on &&
		    uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL) == 0) {
			uart_irq_rx_enable(uart_dev);
			uart_irq_on = true;
		}
		(void)app_service_unlock(&uart_lock);
	}
#endif
}

int app_platform_uart_getc(unsigned char *c)
{
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	if (uart_irq_on) {
		k_spinlock_key_t key = k_spin_lock(&uart_rx_lock);
		uint32_t n = ring_buf_get(&uart_rx_rb, c, 1);

		k_spin_unlock(&uart_rx_lock, key);
		return (n == 1U) ? 0 : -1;
	}
#endif
	return uart_poll_in(uart_dev, c);
}
#endif

#if DT_HAS_CHOSEN(app_uart)
/* The port is shared, so the configuration an application installs is backed
 * by the one found at boot. The application that installed it owns it until it
 * closes the port or is reclaimed; only then is the boot configuration restored,
 * and another application cannot change it meanwhile.
 */
static struct uart_config uart_boot_cfg;
static bool uart_boot_cfg_valid;
static int uart_cfg_owner = -1;

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
	if (id >= 0 && id < MAX_APPS) {
		atomic_clear_bit(&uart_users, id);
	}
	if (app_service_lock(&uart_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
	if (uart_cfg_owner == id) {
		if (uart_boot_cfg_valid) {
			(void)uart_configure(uart_dev, &uart_boot_cfg);
		}
		uart_cfg_owner = -1;
	}
	(void)app_service_unlock(&uart_lock);
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

	int id = app_platform_id_current();

	if (app_service_lock(&uart_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	if (uart_cfg_owner >= 0 && uart_cfg_owner != id) {
		(void)app_service_unlock(&uart_lock);
		return -EBUSY;
	}
	uart_save_boot_cfg();

	int err = uart_configure(uart_dev, &cfg);

	if (err == 0) {
		uart_cfg_owner = id;
	}
	(void)app_service_unlock(&uart_lock);
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
	uart_restore_boot_cfg(app_platform_id_current());
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
	uart_join();
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
	int err;

	uart_join();
	while (n < len && app_platform_uart_getc(&buf[n]) == 0) {
		n++;
	}
	err = uart_err_check(uart_dev);
	if (err > 0) {
		atomic_or(&uart_errs, err & (APP_UART_ERR_OVERRUN | APP_UART_ERR_PARITY |
					     APP_UART_ERR_FRAMING | APP_UART_ERR_BREAK));
	}
	return (int)n;
#else
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

int z_impl_app_svc_uart_set_flow(int rtscts)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(app_uart)
	struct uart_config cfg;
	int id = app_platform_id_current();
	int err;

	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}
	if (app_service_lock(&uart_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	if (uart_cfg_owner >= 0 && uart_cfg_owner != id) {
		(void)app_service_unlock(&uart_lock);
		return -EBUSY;
	}
	uart_save_boot_cfg();
	err = uart_config_get(uart_dev, &cfg);
	if (err == 0) {
		cfg.flow_ctrl = (rtscts != 0) ? UART_CFG_FLOW_CTRL_RTS_CTS
					      : UART_CFG_FLOW_CTRL_NONE;
		err = uart_configure(uart_dev, &cfg);
	}
	if (err == 0) {
		uart_cfg_owner = id;
	}
	(void)app_service_unlock(&uart_lock);
	return err;
#else
	ARG_UNUSED(rtscts);
	return -ENODEV;
#endif
}

int z_impl_app_svc_uart_errors(void)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(app_uart)
	int err = uart_err_check(uart_dev);

	if (err > 0) {
		atomic_or(&uart_errs, err & (APP_UART_ERR_OVERRUN | APP_UART_ERR_PARITY |
					     APP_UART_ERR_FRAMING | APP_UART_ERR_BREAK));
	}
	return (int)atomic_clear(&uart_errs);
#else
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

static inline int z_vrfy_app_svc_i2c_transfer(uint16_t addr, const struct app_i2c_msg *msgs,
					      size_t num, uint8_t *buf, size_t buf_len)
{
	struct app_i2c_msg copy[APP_I2C_MSGS_MAX];

	if (num == 0U || num > APP_I2C_MSGS_MAX) {
		return -EINVAL;
	}
	K_OOPS(k_usermode_from_copy(copy, msgs, num * sizeof(copy[0])));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, buf_len));
	return z_impl_app_svc_i2c_transfer(addr, copy, num, buf, buf_len);
}
#include <zephyr/syscalls/app_svc_i2c_transfer_mrsh.c>

static inline int z_vrfy_app_svc_i2c_configure(uint8_t speed)
{
	return z_impl_app_svc_i2c_configure(speed);
}
#include <zephyr/syscalls/app_svc_i2c_configure_mrsh.c>

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

static inline int z_vrfy_app_svc_spi_transceive(const uint8_t *tx, size_t tx_len,
						uint8_t *rx, size_t rx_len)
{
	if (tx != NULL) {
		K_OOPS(K_SYSCALL_MEMORY_READ(tx, tx_len));
	}
	if (rx != NULL) {
		K_OOPS(K_SYSCALL_MEMORY_WRITE(rx, rx_len));
	}
	return z_impl_app_svc_spi_transceive(tx, tx_len, rx, rx_len);
}
#include <zephyr/syscalls/app_svc_spi_transceive_mrsh.c>

static inline int z_vrfy_app_svc_spi_configure(uint32_t freq_hz, uint8_t flags,
					       uint8_t word_bits, uint8_t cs)
{
	return z_impl_app_svc_spi_configure(freq_hz, flags, word_bits, cs);
}
#include <zephyr/syscalls/app_svc_spi_configure_mrsh.c>

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

static inline int z_vrfy_app_svc_uart_set_flow(int rtscts)
{
	return z_impl_app_svc_uart_set_flow(rtscts);
}
#include <zephyr/syscalls/app_svc_uart_set_flow_mrsh.c>

static inline int z_vrfy_app_svc_uart_errors(void)
{
	return z_impl_app_svc_uart_errors();
}
#include <zephyr/syscalls/app_svc_uart_errors_mrsh.c>
#endif

#if DT_HAS_CHOSEN(app_uart)
static int uart_service_init(void)
{
	return app_platform_on_reclaim(uart_restore_boot_cfg);
}
SYS_INIT(uart_service_init, APPLICATION, 99);
#endif
