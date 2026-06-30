/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared general-I/O services: status LEDs, button/input events and a per-app
 * checkpoint store. The platform owns the LED pins and the button; an
 * application drives only the LED pins it was granted and receives button
 * presses one at a time. The checkpoint store lives outside any application's
 * memory domain, so a value survives the application crashing and restarting.
 * Each hardware service builds to a safe stub when its devicetree node is
 * absent.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS

static inline bool app_has_cap(uint32_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* ---- status LEDs ---- */

#if DT_HAS_ALIAS(led0)
static const struct gpio_dt_spec led[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
#if DT_HAS_ALIAS(led1)
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
#endif
#if DT_HAS_ALIAS(led2)
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
#endif
};
#define LED_COUNT ARRAY_SIZE(led)

/* Per-pin grant: bit N set means an application may drive LED pin N. */
static uint32_t led_grant[MAX_APPS];

static int led_init(void)
{
	for (int i = 0; i < (int)LED_COUNT; i++) {
		if (gpio_is_ready_dt(&led[i])) {
			(void)gpio_pin_configure_dt(&led[i], GPIO_OUTPUT_INACTIVE);
		}
	}
	return 0;
}
SYS_INIT(led_init, APPLICATION, 90);

void app_platform_grant_led(int id, uint32_t pin_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		led_grant[id] = pin_mask;
	}
}

int z_impl_svc_led_set(int pin, int on)
{
	if (!app_has_cap(APP_CAP_LED)) {
		return -EPERM;
	}
	if (pin < 0 || pin >= (int)LED_COUNT || !gpio_is_ready_dt(&led[pin])) {
		return -ENODEV;
	}
	return gpio_pin_set_dt(&led[pin], on ? 1 : 0);
}

int z_impl_svc_led_count(void)
{
	return (int)LED_COUNT;
}
#else  /* no led alias */
void app_platform_grant_led(int id, uint32_t pin_mask)
{
	ARG_UNUSED(id); ARG_UNUSED(pin_mask);
}

int z_impl_svc_led_set(int pin, int on)
{
	ARG_UNUSED(pin); ARG_UNUSED(on);
	return -ENODEV;
}

int z_impl_svc_led_count(void)
{
	return 0;
}
#endif /* DT_HAS_ALIAS(led0) */

/* ---- button / input events ---- */

#if DT_HAS_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;
static K_SEM_DEFINE(press_sem, 0, 16);
static atomic_t press_count;

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev); ARG_UNUSED(cb); ARG_UNUSED(pins);
	atomic_inc(&press_count);
	k_sem_give(&press_sem);
}

static int button_init(void)
{
	if (!gpio_is_ready_dt(&button)) {
		return 0;
	}
	(void)gpio_pin_configure_dt(&button, GPIO_INPUT);
	(void)gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
	(void)gpio_add_callback(button.port, &button_cb);
	return 0;
}
SYS_INIT(button_init, APPLICATION, 90);

int z_impl_svc_button_wait(int32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_INPUT)) {
		return -EPERM;
	}

	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	if (k_sem_take(&press_sem, t) != 0) {
		return -EAGAIN;
	}
	return (int)atomic_get(&press_count);
}
#else  /* no sw0 alias */
int z_impl_svc_button_wait(int32_t timeout_ms)
{
	ARG_UNUSED(timeout_ms);
	return -ENODEV;
}
#endif /* DT_HAS_ALIAS(sw0) */

/* ---- per-app checkpoint store ---- */

/*
 * One retained value per application slot, kept on the platform side outside any
 * application's memory domain so it survives a crash and restart. Capability:
 * the checkpoint is part of every application's own state, so it needs no grant.
 */
static uint32_t checkpoints[MAX_APPS];

void z_impl_svc_checkpoint_set(uint32_t value)
{
	int id = app_platform_id_current();

	if (id >= 0 && id < MAX_APPS) {
		checkpoints[id] = value;
	}
}

uint32_t z_impl_svc_checkpoint_get(void)
{
	int id = app_platform_id_current();

	return (id >= 0 && id < MAX_APPS) ? checkpoints[id] : 0U;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_led_set(int pin, int on)
{
#if DT_HAS_ALIAS(led0)
	/* Reject any LED pin the calling application was not granted. */
	int id = app_platform_id_current();

	if (pin < 0 || pin >= (int)LED_COUNT || id < 0 || id >= MAX_APPS ||
	    (led_grant[id] & BIT(pin)) == 0U) {
		return -EPERM;
	}
#endif
	return z_impl_svc_led_set(pin, on);
}
#include <zephyr/syscalls/svc_led_set_mrsh.c>

static inline int z_vrfy_svc_led_count(void)
{
	return z_impl_svc_led_count();
}
#include <zephyr/syscalls/svc_led_count_mrsh.c>

static inline int z_vrfy_svc_button_wait(int32_t timeout_ms)
{
	return z_impl_svc_button_wait(timeout_ms);
}
#include <zephyr/syscalls/svc_button_wait_mrsh.c>

static inline void z_vrfy_svc_checkpoint_set(uint32_t value)
{
	z_impl_svc_checkpoint_set(value);
}
#include <zephyr/syscalls/svc_checkpoint_set_mrsh.c>

static inline uint32_t z_vrfy_svc_checkpoint_get(void)
{
	return z_impl_svc_checkpoint_get();
}
#include <zephyr/syscalls/svc_checkpoint_get_mrsh.c>
#endif /* CONFIG_USERSPACE */
