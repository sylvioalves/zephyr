/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_surface.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/internal/syscall_handler.h>

LOG_MODULE_DECLARE(behavior_sandbox, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * Controllable output pin: a free GPIO the sandbox is permitted to toggle.
 * Wire an LED/scope here to observe the happy-path capability call.
 */
static const struct gpio_dt_spec out_pin =
	GPIO_DT_SPEC_GET(DT_ALIAS(sandbox_out), gpios);

static const struct device *const die_temp =
	DEVICE_DT_GET(DT_ALIAS(sandbox_temp));

int cap_surface_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&out_pin)) {
		LOG_ERR("sandbox output pin not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&out_pin, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("failed to configure output pin: %d", ret);
		return ret;
	}

	if (!device_is_ready(die_temp)) {
		LOG_ERR("die temperature sensor not ready");
		return -ENODEV;
	}

	return 0;
}

int z_impl_cap_led_set(bool on)
{
	return gpio_pin_set_dt(&out_pin, on ? 1 : 0);
}

int z_impl_cap_sensor_read(int32_t *milli_c)
{
	struct sensor_value val;
	int ret;

	ret = sensor_sample_fetch(die_temp);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &val);
	if (ret < 0) {
		return ret;
	}

	*milli_c = val.val1 * 1000 + val.val2 / 1000;
	return 0;
}

void z_impl_cap_sleep(uint32_t ms)
{
	k_msleep((int32_t)ms);
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_cap_led_set(bool on)
{
	return z_impl_cap_led_set(on);
}
#include <zephyr/syscalls/cap_led_set_mrsh.c>

static inline int z_vrfy_cap_sensor_read(int32_t *milli_c)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(milli_c, sizeof(*milli_c)));
	return z_impl_cap_sensor_read(milli_c);
}
#include <zephyr/syscalls/cap_sensor_read_mrsh.c>

static inline void z_vrfy_cap_sleep(uint32_t ms)
{
	z_impl_cap_sleep(ms);
}
#include <zephyr/syscalls/cap_sleep_mrsh.c>

#endif /* CONFIG_USERSPACE */
