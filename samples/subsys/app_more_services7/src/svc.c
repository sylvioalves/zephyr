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

/* The platform owns the button (input) and several status LEDs (outputs). */
static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

static const struct gpio_dt_spec led[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
};
#define LED_COUNT ARRAY_SIZE(led)

/* Per-pin grant: bit N set means the app may drive LED pin N. */
static uint32_t led_grant;

void svc_led_grant(uint32_t pin_mask)
{
	led_grant = pin_mask;
}

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
	for (int i = 0; i < (int)LED_COUNT; i++) {
		if (gpio_is_ready_dt(&led[i])) {
			(void)gpio_pin_configure_dt(&led[i], GPIO_OUTPUT_INACTIVE);
		}
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

int z_impl_svc_led_set(int pin, int on)
{
	if (pin < 0 || pin >= (int)LED_COUNT || !gpio_is_ready_dt(&led[pin])) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(&led[pin], on ? 1 : 0);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_button_wait(int32_t timeout_ms)
{
	return z_impl_svc_button_wait(timeout_ms);
}
#include <zephyr/syscalls/svc_button_wait_mrsh.c>

static inline int z_vrfy_svc_led_set(int pin, int on)
{
	/* Reject any LED pin the app was not granted. */
	if (pin < 0 || pin >= (int)LED_COUNT || (led_grant & BIT(pin)) == 0U) {
		return -EPERM;
	}
	return z_impl_svc_led_set(pin, on);
}
#include <zephyr/syscalls/svc_led_set_mrsh.c>
#endif
