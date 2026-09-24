/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared hardware counter service. The platform owns the counter instances and
 * hands an application a granted one by index, so an application can time an
 * interval against real hardware rather than the scheduler. Reading is the
 * whole surface: an application never receives the device, and alarms stay with
 * the platform because a callback cannot cross into user mode.
 *
 * The service builds to nothing when the board has no counter node, so a board
 * without the peripheral still builds.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

#include "app_grants.h"

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* Every enabled counter node on the board, in devicetree order. */
#define COUNTER_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const counters[] = {
	DT_FOREACH_STATUS_OKAY(espressif_esp32_counter, COUNTER_ENTRY)
};

#define COUNTER_COUNT ARRAY_SIZE(counters)

static uint32_t counter_grant[MAX_APPS];
/* Bit N set means this application issued the last start of counter N. */
static uint32_t counter_started[MAX_APPS];
static K_MUTEX_DEFINE(counter_lock);

void app_platform_grant_counter(int id, uint32_t idx_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		counter_grant[id] = idx_mask;
	}
}

void app_platform_clear_counter_grants(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	counter_grant[id] = 0U;
	if (app_svc_lock(&counter_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
	for (int i = 0; i < (int)COUNTER_COUNT; i++) {
		if ((counter_started[id] & BIT(i)) != 0U) {
			(void)counter_stop(counters[i]);
		}
	}
	counter_started[id] = 0U;
	(void)app_svc_unlock(&counter_lock);
}

/* Resolve a granted counter index to its device, separating "no capability"
 * from "this instance was not assigned to you".
 */
static int granted_counter(int idx, const struct device **out)
{
	if (!app_has_cap(APP_CAP_COUNTER_DEV)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || idx < 0 || idx >= (int)COUNTER_COUNT) {
		return -EINVAL;
	}
	if ((counter_grant[id] & BIT(idx)) == 0U) {
		return -EACCES;
	}
	if (!device_is_ready(counters[idx])) {
		return -ENODEV;
	}
	*out = counters[idx];
	return 0;
}

int z_impl_svc_counter_count(void)
{
	if (!app_has_cap(APP_CAP_COUNTER_DEV)) {
		return -EPERM;
	}
	return (int)COUNTER_COUNT;
}

int z_impl_svc_counter_start(int idx)
{
	const struct device *dev;
	int rc = granted_counter(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	if (app_svc_lock(&counter_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	rc = counter_start(dev);
	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			counter_started[i] &= ~BIT(idx);
		}
		counter_started[app_platform_id_current()] |= BIT(idx);
	}
	(void)app_svc_unlock(&counter_lock);
	return rc;
}

int z_impl_svc_counter_stop(int idx)
{
	const struct device *dev;
	int rc = granted_counter(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	if (app_svc_lock(&counter_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	rc = counter_stop(dev);
	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			counter_started[i] &= ~BIT(idx);
		}
	}
	(void)app_svc_unlock(&counter_lock);
	return rc;
}

int z_impl_svc_counter_read(int idx, uint32_t *ticks)
{
	const struct device *dev;
	uint32_t val = 0;
	int rc = granted_counter(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	rc = counter_get_value(dev, &val);
	if (rc != 0) {
		return rc;
	}
	*ticks = val;
	return 0;
}

int z_impl_svc_counter_frequency(int idx, uint32_t *hz)
{
	const struct device *dev;
	int rc = granted_counter(idx, &dev);

	if (rc != 0) {
		return rc;
	}
	*hz = counter_get_frequency(dev);
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_counter_count(void)
{
	return z_impl_svc_counter_count();
}
#include <zephyr/syscalls/svc_counter_count_mrsh.c>

static inline int z_vrfy_svc_counter_start(int idx)
{
	return z_impl_svc_counter_start(idx);
}
#include <zephyr/syscalls/svc_counter_start_mrsh.c>

static inline int z_vrfy_svc_counter_stop(int idx)
{
	return z_impl_svc_counter_stop(idx);
}
#include <zephyr/syscalls/svc_counter_stop_mrsh.c>

/* Bounce through a supervisor value rather than letting the driver write user
 * memory, as the other reading services do.
 */
static inline int z_vrfy_svc_counter_read(int idx, uint32_t *ticks)
{
	uint32_t val = 0;
	int rc = z_impl_svc_counter_read(idx, &val);

	if (rc == 0 && k_usermode_to_copy(ticks, &val, sizeof(val)) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/svc_counter_read_mrsh.c>

static inline int z_vrfy_svc_counter_frequency(int idx, uint32_t *hz)
{
	uint32_t val = 0;
	int rc = z_impl_svc_counter_frequency(idx, &val);

	if (rc == 0 && k_usermode_to_copy(hz, &val, sizeof(val)) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/svc_counter_frequency_mrsh.c>
#endif /* CONFIG_USERSPACE */
