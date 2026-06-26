/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

/* The platform owns both the button (input) and the status LED (output). */
static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct gpio_callback button_cb;

/* Press events are counted in S-mode and handed to the app one at a time. */
static K_SEM_DEFINE(press_sem, 0, 16);
static atomic_t press_count;

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev); ARG_UNUSED(cb); ARG_UNUSED(pins);

	atomic_inc(&press_count);
	k_sem_give(&press_sem);
}

void svc_input_install(void)
{
	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}

	if (!gpio_is_ready_dt(&button)) {
		return;
	}

	(void)gpio_pin_configure_dt(&button, GPIO_INPUT);
	(void)gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
	(void)gpio_add_callback(button.port, &button_cb);
}

int z_impl_svc_button_wait(int32_t timeout_ms)
{
	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	if (k_sem_take(&press_sem, t) != 0) {
		return -EAGAIN;
	}

	return (int)atomic_get(&press_count);
}

int z_impl_svc_led_set(int on)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(&led, on ? 1 : 0);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_button_wait(int32_t timeout_ms)
{
	return z_impl_svc_button_wait(timeout_ms);
}
#include <zephyr/syscalls/svc_button_wait_mrsh.c>

static inline int z_vrfy_svc_led_set(int on)
{
	return z_impl_svc_led_set(on);
}
#include <zephyr/syscalls/svc_led_set_mrsh.c>
#endif
