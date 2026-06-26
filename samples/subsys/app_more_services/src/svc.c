/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/random/random.h>
#include <zephyr/internal/syscall_handler.h>

/* Devices the platform owns. */
static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_ALIAS(more_led), gpios);
static const struct device *const counter_dev =
	DEVICE_DT_GET(DT_ALIAS(more_counter));

/* One app's timer-expiry signal (single-app demo; per-app in a full platform). */
static struct k_sem timer_sem;
static struct counter_alarm_cfg alarm;

void svc_init(void)
{
	if (gpio_is_ready_dt(&button)) {
		gpio_pin_configure_dt(&button, GPIO_INPUT);
	}
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	k_sem_init(&timer_sem, 0, 1);
	if (device_is_ready(counter_dev)) {
		counter_start(counter_dev);
	}
}

/* ---- RNG ---- */
int z_impl_svc_random(uint8_t *buf, size_t len)
{
	sys_rand_get(buf, len);
	return 0;
}

/* ---- GPIO input ---- */
int z_impl_svc_button(void)
{
	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}
	return gpio_pin_get_dt(&button);
}

/* ---- GPIO output ---- */
int z_impl_svc_led(int on)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}
	return gpio_pin_set_dt(&led, on ? 1 : 0);
}

/* ---- hardware counter as a one-shot timer ---- */
static void alarm_cb(const struct device *dev, uint8_t chan, uint32_t ticks,
		     void *user)
{
	ARG_UNUSED(dev); ARG_UNUSED(chan); ARG_UNUSED(ticks); ARG_UNUSED(user);
	/* Expiry arrives in ISR context; signal the app (the event seam). */
	k_sem_give(&timer_sem);
}

int z_impl_svc_timer_start(uint32_t ms)
{
	if (!device_is_ready(counter_dev)) {
		return -ENODEV;
	}
	alarm.flags = 0;
	alarm.ticks = counter_us_to_ticks(counter_dev, (uint64_t)ms * 1000);
	alarm.callback = alarm_cb;
	alarm.user_data = NULL;
	k_sem_reset(&timer_sem);
	return counter_set_channel_alarm(counter_dev, 0, &alarm);
}

int z_impl_svc_timer_wait(int32_t timeout_ms)
{
	return (k_sem_take(&timer_sem, K_MSEC(timeout_ms)) == 0) ? 0 : -EAGAIN;
}

/* ---- time ---- */
uint32_t z_impl_svc_uptime(void)
{
	return (uint32_t)k_uptime_get();
}

uint32_t z_impl_svc_cycles(void)
{
	return k_cycle_get_32();
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_random(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_random(buf, len);
}
#include <zephyr/syscalls/svc_random_mrsh.c>

static inline int z_vrfy_svc_button(void)
{
	return z_impl_svc_button();
}
#include <zephyr/syscalls/svc_button_mrsh.c>

static inline int z_vrfy_svc_led(int on)
{
	return z_impl_svc_led(on);
}
#include <zephyr/syscalls/svc_led_mrsh.c>

static inline int z_vrfy_svc_timer_start(uint32_t ms)
{
	return z_impl_svc_timer_start(ms);
}
#include <zephyr/syscalls/svc_timer_start_mrsh.c>

static inline int z_vrfy_svc_timer_wait(int32_t timeout_ms)
{
	return z_impl_svc_timer_wait(timeout_ms);
}
#include <zephyr/syscalls/svc_timer_wait_mrsh.c>

static inline uint32_t z_vrfy_svc_uptime(void)
{
	return z_impl_svc_uptime();
}
#include <zephyr/syscalls/svc_uptime_mrsh.c>

static inline uint32_t z_vrfy_svc_cycles(void)
{
	return z_impl_svc_cycles();
}
#include <zephyr/syscalls/svc_cycles_mrsh.c>
#endif
