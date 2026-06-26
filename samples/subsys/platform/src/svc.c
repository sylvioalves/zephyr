/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"
#include "platform.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

extern int manager_id_of_current(void);
extern void manager_app_feed(void);

#define FLASH_SLOT_MAX 64

/* The platform owns every device; apps reach them only through these services. */
static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(DT_ALIAS(app_pwm));
static const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(counter_rtc));
static const struct flash_area *fa;

void svc_devices_init(void)
{
	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	(void)flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);

	/* Seed the wall clock so reads return a sensible time. */
	if (device_is_ready(rtc)) {
		struct rtc_time t = {
			.tm_year = 2026 - 1900, .tm_mon = 5, .tm_mday = 26,
			.tm_hour = 12, .tm_min = 0, .tm_sec = 0,
			.tm_wday = -1, .tm_yday = -1, .tm_isdst = -1,
		};
		(void)rtc_set_time(rtc, &t);
	}
}

/* ---- always-available services ---- */

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", manager_id_of_current(), msg);
}

void z_impl_svc_heartbeat(void)
{
	manager_app_feed();
}

/* ---- capability-gated services ---- */

int z_impl_svc_sensor_read(int32_t *milli_c)
{
	if ((manager_caps_of_current() & CAP_SENSOR) == 0U) {
		return -EPERM;
	}
	if (!device_is_ready(die_temp) || sensor_sample_fetch(die_temp) != 0) {
		return -EIO;
	}
	struct sensor_value v;

	if (sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v) != 0) {
		return -EIO;
	}
	*milli_c = v.val1 * 1000 + v.val2 / 1000;
	return 0;
}

int z_impl_svc_led_set(int on)
{
	if ((manager_caps_of_current() & CAP_GPIO) == 0U) {
		return -EPERM;
	}
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}
	return gpio_pin_set_dt(&led, on ? 1 : 0);
}

int z_impl_svc_pwm_set(uint8_t percent)
{
	if ((manager_caps_of_current() & CAP_PWM) == 0U) {
		return -EPERM;
	}
	if (!pwm_is_ready_dt(&pwm)) {
		return -ENODEV;
	}
	if (percent > 100) {
		percent = 100;
	}
	uint32_t pulse = (uint32_t)((uint64_t)pwm.period * percent / 100);

	return pwm_set_pulse_dt(&pwm, pulse);
}

int z_impl_svc_flash_write(uint32_t off, const uint8_t *buf, size_t len)
{
	if ((manager_caps_of_current() & CAP_FLASH) == 0U) {
		return -EPERM;
	}
	if (fa == NULL || len > FLASH_SLOT_MAX ||
	    (uint64_t)off + len > (uint64_t)fa->fa_size) {
		return -EINVAL;
	}
	int rc = flash_area_write(fa, off, buf, len);

	return (rc == 0) ? (int)len : rc;
}

int z_impl_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	if ((manager_caps_of_current() & CAP_FLASH) == 0U) {
		return -EPERM;
	}
	if (fa == NULL || len > FLASH_SLOT_MAX ||
	    (uint64_t)off + len > (uint64_t)fa->fa_size) {
		return -EINVAL;
	}
	int rc = flash_area_read(fa, off, buf, len);

	return (rc == 0) ? (int)len : rc;
}

int z_impl_svc_rtc_now(uint32_t *epoch_sec)
{
	if ((manager_caps_of_current() & CAP_RTC) == 0U) {
		return -EPERM;
	}
	if (!device_is_ready(rtc)) {
		return -ENODEV;
	}
	struct rtc_time t;

	if (rtc_get_time(rtc, &t) != 0) {
		return -EIO;
	}
	/* A coarse seconds-of-day value is enough to show the clock advancing. */
	*epoch_sec = (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec);
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_sensor_read(int32_t *milli_c)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(milli_c, sizeof(*milli_c)));
	return z_impl_svc_sensor_read(milli_c);
}
#include <zephyr/syscalls/svc_sensor_read_mrsh.c>

static inline void z_vrfy_svc_log(const char *msg)
{
	char buf[48];

	if (k_usermode_string_copy(buf, (char *)msg, sizeof(buf)) == 0) {
		z_impl_svc_log(buf);
	}
}
#include <zephyr/syscalls/svc_log_mrsh.c>

static inline void z_vrfy_svc_heartbeat(void)
{
	z_impl_svc_heartbeat();
}
#include <zephyr/syscalls/svc_heartbeat_mrsh.c>

static inline int z_vrfy_svc_led_set(int on)
{
	return z_impl_svc_led_set(on);
}
#include <zephyr/syscalls/svc_led_set_mrsh.c>

static inline int z_vrfy_svc_pwm_set(uint8_t percent)
{
	return z_impl_svc_pwm_set(percent);
}
#include <zephyr/syscalls/svc_pwm_set_mrsh.c>

static inline int z_vrfy_svc_flash_write(uint32_t off, const uint8_t *buf, size_t len)
{
	if (len > FLASH_SLOT_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_flash_write(off, buf, len);
}
#include <zephyr/syscalls/svc_flash_write_mrsh.c>

static inline int z_vrfy_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	if (len > FLASH_SLOT_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_flash_read(off, buf, len);
}
#include <zephyr/syscalls/svc_flash_read_mrsh.c>

static inline int z_vrfy_svc_rtc_now(uint32_t *epoch_sec)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(epoch_sec, sizeof(*epoch_sec)));
	return z_impl_svc_rtc_now(epoch_sec);
}
#include <zephyr/syscalls/svc_rtc_now_mrsh.c>
#endif
