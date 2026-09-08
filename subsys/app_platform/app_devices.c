/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Index-keyed wrappers over the remaining device classes an application may
 * reach: the real-time clock, the 1-Wire bus, battery state, regulators and
 * stepper motors. Each follows the platform's shape - the application names an
 * instance by index, never receives a device, and is refused before any
 * hardware is touched when it holds neither the capability nor a grant.
 *
 * Reading the clock and battery state is device-wide and needs only a
 * capability. Setting the clock is separated behind its own capability because
 * it changes state every other application observes.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>
#include <string.h>

#include "app_grants.h"

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* ---- real-time clock ---- */

#ifdef CONFIG_APP_PLATFORM_RTC
#include <zephyr/drivers/rtc.h>

static const struct device *const rtc_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_rtc));

int z_impl_svc_rtcdev_get(struct svc_rtc_time *out)
{
	if (!app_has_cap(APP_CAP_RTC)) {
		return -EPERM;
	}
	if (!device_is_ready(rtc_dev)) {
		return -ENODEV;
	}

	struct rtc_time t;
	int rc = rtc_get_time(rtc_dev, &t);

	if (rc != 0) {
		return rc;
	}
	*out = (struct svc_rtc_time){
		.year = t.tm_year + 1900,
		.month = (uint8_t)(t.tm_mon + 1),
		.day = (uint8_t)t.tm_mday,
		.hour = (uint8_t)t.tm_hour,
		.minute = (uint8_t)t.tm_min,
		.second = (uint8_t)t.tm_sec,
	};
	return 0;
}

/* Setting the clock is device-wide, so it takes a capability of its own rather
 * than riding on the one that only reads it.
 */
int z_impl_svc_rtcdev_set(const struct svc_rtc_time *in)
{
	if (!app_has_cap(APP_CAP_RTC_SET)) {
		return -EPERM;
	}
	if (!device_is_ready(rtc_dev)) {
		return -ENODEV;
	}
	if (in->month < 1U || in->month > 12U || in->day < 1U || in->day > 31U ||
	    in->hour > 23U || in->minute > 59U || in->second > 59U ||
	    in->year < 1900U) {
		return -EINVAL;
	}

	struct rtc_time t = {
		.tm_year = (int)in->year - 1900,
		.tm_mon = (int)in->month - 1,
		.tm_mday = (int)in->day,
		.tm_hour = (int)in->hour,
		.tm_min = (int)in->minute,
		.tm_sec = (int)in->second,
		.tm_wday = -1,
		.tm_yday = -1,
		.tm_isdst = -1,
		.tm_nsec = 0,
	};

	return rtc_set_time(rtc_dev, &t);
}
#endif /* CONFIG_APP_PLATFORM_RTC */

/* ---- 1-Wire ---- */

#ifdef CONFIG_APP_PLATFORM_W1
#include <zephyr/drivers/w1.h>

static const struct device *const w1_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_w1));

int z_impl_svc_w1_reset(void)
{
	if (!app_has_cap(APP_CAP_W1)) {
		return -EPERM;
	}
	if (!device_is_ready(w1_dev)) {
		return -ENODEV;
	}
	return w1_reset_bus(w1_dev);
}

int z_impl_svc_w1_read_byte(void)
{
	if (!app_has_cap(APP_CAP_W1)) {
		return -EPERM;
	}
	if (!device_is_ready(w1_dev)) {
		return -ENODEV;
	}
	return w1_read_byte(w1_dev);
}

int z_impl_svc_w1_write_byte(uint8_t byte)
{
	if (!app_has_cap(APP_CAP_W1)) {
		return -EPERM;
	}
	if (!device_is_ready(w1_dev)) {
		return -ENODEV;
	}
	return w1_write_byte(w1_dev, byte);
}
#endif /* CONFIG_APP_PLATFORM_W1 */

/* ---- battery state ---- */

#ifdef CONFIG_APP_PLATFORM_POWER
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/fuel_gauge.h>

static const struct device *const charger_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_charger));
static const struct device *const gauge_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_fuel_gauge));

/* Read-only by design: an application observes the battery, and changing the
 * charge policy stays with the system.
 */
int z_impl_svc_charger_online(void)
{
	if (!app_has_cap(APP_CAP_POWER)) {
		return -EPERM;
	}
	if (!device_is_ready(charger_dev)) {
		return -ENODEV;
	}

	union charger_propval val;
	int rc = charger_get_prop(charger_dev, CHARGER_PROP_ONLINE, &val);

	return rc != 0 ? rc : (int)val.online;
}

int z_impl_svc_battery_percent(void)
{
	if (!app_has_cap(APP_CAP_POWER)) {
		return -EPERM;
	}
	if (!device_is_ready(gauge_dev)) {
		return -ENODEV;
	}

	union fuel_gauge_prop_val val;
	int rc = fuel_gauge_get_prop(gauge_dev,
				     FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE_PCT, &val);

	return rc != 0 ? rc : (int)val.relative_state_of_charge_pct;
}
#endif /* CONFIG_APP_PLATFORM_POWER */

/* ---- regulators ---- */

#ifdef CONFIG_APP_PLATFORM_REGULATOR
#include <zephyr/drivers/regulator.h>

/* A regulator device is a child of its controller node, so the controllers are
 * walked and each of their children contributes one instance.
 */
#define REG_CHILD(node)  DEVICE_DT_GET(node),
#define REG_ENTRY(node)  DT_FOREACH_CHILD_STATUS_OKAY(node, REG_CHILD)

static const struct device *const regulators[] = {
	DT_FOREACH_STATUS_OKAY(zephyr_fake_regulator, REG_ENTRY)
};

#define REG_COUNT ARRAY_SIZE(regulators)

static uint32_t reg_grant[MAX_APPS];

void app_platform_grant_regulator(int id, uint32_t idx_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		reg_grant[id] = idx_mask;
	}
}

void app_platform_clear_regulator_grants(int id)
{
	if (id >= 0 && id < MAX_APPS) {
		reg_grant[id] = 0U;
	}
}

static int granted_regulator(int idx, const struct device **out)
{
	if (!app_has_cap(APP_CAP_REGULATOR)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || idx < 0 || idx >= (int)REG_COUNT) {
		return -EINVAL;
	}
	if ((reg_grant[id] & BIT(idx)) == 0U) {
		return -EACCES;
	}
	if (!device_is_ready(regulators[idx])) {
		return -ENODEV;
	}
	*out = regulators[idx];
	return 0;
}

int z_impl_svc_regulator_count(void)
{
	if (!app_has_cap(APP_CAP_REGULATOR)) {
		return -EPERM;
	}
	return (int)REG_COUNT;
}

int z_impl_svc_regulator_enable(int idx, int on)
{
	const struct device *dev;
	int rc = granted_regulator(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	return on != 0 ? regulator_enable(dev) : regulator_disable(dev);
}
#endif /* CONFIG_APP_PLATFORM_REGULATOR */

/* ---- stepper motors ---- */

#ifdef CONFIG_APP_PLATFORM_STEPPER
#include <zephyr/drivers/stepper/stepper_ctrl.h>

#define STEP_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const steppers[] = {
	DT_FOREACH_STATUS_OKAY(zephyr_fake_stepper_ctrl, STEP_ENTRY)
};

#define STEP_COUNT ARRAY_SIZE(steppers)

static uint32_t step_grant[MAX_APPS];

void app_platform_grant_stepper(int id, uint32_t idx_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		step_grant[id] = idx_mask;
	}
}

void app_platform_clear_stepper_grants(int id)
{
	if (id >= 0 && id < MAX_APPS) {
		step_grant[id] = 0U;
	}
}

static int granted_stepper(int idx, const struct device **out)
{
	if (!app_has_cap(APP_CAP_STEPPER)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || idx < 0 || idx >= (int)STEP_COUNT) {
		return -EINVAL;
	}
	if ((step_grant[id] & BIT(idx)) == 0U) {
		return -EACCES;
	}
	if (!device_is_ready(steppers[idx])) {
		return -ENODEV;
	}
	*out = steppers[idx];
	return 0;
}

int z_impl_svc_stepper_count(void)
{
	if (!app_has_cap(APP_CAP_STEPPER)) {
		return -EPERM;
	}
	return (int)STEP_COUNT;
}

/* The subsystem separates the motor from its controller: energizing belongs to
 * the stepper device and motion to the controller. An application is granted a
 * controller, so stopping it is expressed as a stop rather than a de-energize.
 */
int z_impl_svc_stepper_stop(int idx)
{
	const struct device *dev;
	int rc = granted_stepper(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	return stepper_ctrl_stop(dev);
}

/* Movement is fire-and-forget: completion is reported through a callback, and
 * a callback cannot cross into user mode, so an application polls the position
 * instead of being told when the move ends.
 */
int z_impl_svc_stepper_move_by(int idx, int32_t micro_steps)
{
	const struct device *dev;
	int rc = granted_stepper(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	return stepper_ctrl_move_by(dev, micro_steps);
}

int z_impl_svc_stepper_position(int idx, int32_t *pos)
{
	const struct device *dev;
	int32_t val = 0;
	int rc = granted_stepper(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	rc = stepper_ctrl_get_actual_position(dev, &val);
	if (rc != 0) {
		return rc;
	}
	*pos = val;
	return 0;
}
#endif /* CONFIG_APP_PLATFORM_STEPPER */

#ifdef CONFIG_USERSPACE
#ifdef CONFIG_APP_PLATFORM_RTC
static inline int z_vrfy_svc_rtcdev_get(struct svc_rtc_time *out)
{
	struct svc_rtc_time tmp;
	int rc = z_impl_svc_rtcdev_get(&tmp);

	if (rc == 0 && k_usermode_to_copy(out, &tmp, sizeof(tmp)) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/svc_rtcdev_get_mrsh.c>

static inline int z_vrfy_svc_rtcdev_set(const struct svc_rtc_time *in)
{
	struct svc_rtc_time tmp;

	if (k_usermode_from_copy(&tmp, (void *)in, sizeof(tmp)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_rtcdev_set(&tmp);
}
#include <zephyr/syscalls/svc_rtcdev_set_mrsh.c>
#endif /* CONFIG_APP_PLATFORM_RTC */

#ifdef CONFIG_APP_PLATFORM_W1
static inline int z_vrfy_svc_w1_reset(void)
{
	return z_impl_svc_w1_reset();
}
#include <zephyr/syscalls/svc_w1_reset_mrsh.c>

static inline int z_vrfy_svc_w1_read_byte(void)
{
	return z_impl_svc_w1_read_byte();
}
#include <zephyr/syscalls/svc_w1_read_byte_mrsh.c>

static inline int z_vrfy_svc_w1_write_byte(uint8_t byte)
{
	return z_impl_svc_w1_write_byte(byte);
}
#include <zephyr/syscalls/svc_w1_write_byte_mrsh.c>
#endif /* CONFIG_APP_PLATFORM_W1 */

#ifdef CONFIG_APP_PLATFORM_POWER
static inline int z_vrfy_svc_charger_online(void)
{
	return z_impl_svc_charger_online();
}
#include <zephyr/syscalls/svc_charger_online_mrsh.c>

static inline int z_vrfy_svc_battery_percent(void)
{
	return z_impl_svc_battery_percent();
}
#include <zephyr/syscalls/svc_battery_percent_mrsh.c>
#endif /* CONFIG_APP_PLATFORM_POWER */

#ifdef CONFIG_APP_PLATFORM_REGULATOR
static inline int z_vrfy_svc_regulator_count(void)
{
	return z_impl_svc_regulator_count();
}
#include <zephyr/syscalls/svc_regulator_count_mrsh.c>

static inline int z_vrfy_svc_regulator_enable(int idx, int on)
{
	return z_impl_svc_regulator_enable(idx, on);
}
#include <zephyr/syscalls/svc_regulator_enable_mrsh.c>
#endif /* CONFIG_APP_PLATFORM_REGULATOR */

#ifdef CONFIG_APP_PLATFORM_STEPPER
static inline int z_vrfy_svc_stepper_count(void)
{
	return z_impl_svc_stepper_count();
}
#include <zephyr/syscalls/svc_stepper_count_mrsh.c>

static inline int z_vrfy_svc_stepper_stop(int idx)
{
	return z_impl_svc_stepper_stop(idx);
}
#include <zephyr/syscalls/svc_stepper_stop_mrsh.c>

static inline int z_vrfy_svc_stepper_move_by(int idx, int32_t micro_steps)
{
	return z_impl_svc_stepper_move_by(idx, micro_steps);
}
#include <zephyr/syscalls/svc_stepper_move_by_mrsh.c>

static inline int z_vrfy_svc_stepper_position(int idx, int32_t *pos)
{
	int32_t val = 0;
	int rc = z_impl_svc_stepper_position(idx, &val);

	if (rc == 0 && k_usermode_to_copy(pos, &val, sizeof(val)) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/svc_stepper_position_mrsh.c>
#endif /* CONFIG_APP_PLATFORM_STEPPER */
#endif /* CONFIG_USERSPACE */
