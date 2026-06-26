/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

#define UART_MAX 64

/* The platform owns the second UART; the app reaches it only as a byte channel. */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* The platform owns the die-temp sensor (open, read-only to apps). */
static const struct device *const temp_dev = DEVICE_DT_GET(DT_ALIAS(die_temp0));

/* The platform installs and owns the watchdog; the app feeds its channel. */
static const struct device *const wdt_dev = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int app_wdt_chan = -1;

void svc_wdt_install(void)
{
	if (!device_is_ready(wdt_dev)) {
		return;
	}

	struct wdt_timeout_cfg cfg = {
		.window.min = 0U,
		.window.max = 8000U,
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	app_wdt_chan = wdt_install_timeout(wdt_dev, &cfg);
	if (app_wdt_chan >= 0) {
		wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
	}
}

int z_impl_svc_uart_write(const uint8_t *buf, size_t len)
{
	if (!device_is_ready(uart_dev) || len > UART_MAX) {
		return -ENODEV;
	}
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, buf[i]);
	}
	return (int)len;
}

int z_impl_svc_uart_read(uint8_t *out)
{
	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}
	int rc = uart_poll_in(uart_dev, out);

	return (rc == 0) ? 1 : -EAGAIN;
}

int z_impl_svc_die_temp(int32_t *milli_c)
{
	if (!device_is_ready(temp_dev)) {
		return -ENODEV;
	}
	int rc = sensor_sample_fetch(temp_dev);

	if (rc != 0) {
		return rc;
	}

	struct sensor_value v;

	rc = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &v);
	if (rc != 0) {
		return rc;
	}

	*milli_c = v.val1 * 1000 + v.val2 / 1000;
	return 0;
}

int z_impl_svc_wdt_feed(void)
{
	if (app_wdt_chan < 0) {
		return -ENODEV;
	}
	return wdt_feed(wdt_dev, app_wdt_chan);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_uart_write(const uint8_t *buf, size_t len)
{
	if (len > UART_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_uart_write(buf, len);
}
#include <zephyr/syscalls/svc_uart_write_mrsh.c>

static inline int z_vrfy_svc_uart_read(uint8_t *out)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_uart_read(out);
}
#include <zephyr/syscalls/svc_uart_read_mrsh.c>

static inline int z_vrfy_svc_die_temp(int32_t *milli_c)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(milli_c, sizeof(*milli_c)));
	return z_impl_svc_die_temp(milli_c);
}
#include <zephyr/syscalls/svc_die_temp_mrsh.c>

static inline int z_vrfy_svc_wdt_feed(void)
{
	return z_impl_svc_wdt_feed();
}
#include <zephyr/syscalls/svc_wdt_feed_mrsh.c>
#endif
