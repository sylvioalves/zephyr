/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The shared GPIO service. The platform owns a set of output lines taken from
 * the board's "leds" devicetree node. An application that holds APP_CAP_GPIO may
 * drive only the lines it was granted (see app_platform_grant_gpio), so the
 * capability admits the service and a per-line mask scopes it to specific pins -
 * the second-level grant pattern. A line an application last drove active is
 * driven inactive when that application is reclaimed.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include "app_events.h"
#include "app_grants.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>
#include <string.h>

#define LEDS_NODE DT_PATH(leds)

#if DT_NODE_EXISTS(LEDS_NODE)
#define GPIO_SPEC(node_id) GPIO_DT_SPEC_GET(node_id, gpios),
#define GPIO_NAME(node_id) \
	DT_PROP_OR(node_id, label, DT_NODE_FULL_NAME(node_id)),

static const struct gpio_dt_spec lines[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(LEDS_NODE, GPIO_SPEC)
};
#define LINE_COUNT ARRAY_SIZE(lines)

static const char *const line_name[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(LEDS_NODE, GPIO_NAME)
};
#else
#define LINE_COUNT 0
#endif

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS
#define IO_NAME_MAX 24
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

static uint32_t line_grant[MAX_APPS];
/* Bit N set means this application last drove line N active. */
static uint32_t line_active[MAX_APPS];
static K_MUTEX_DEFINE(line_lock);

/* Drop every line grant of a slot that is being handed to a new application
 * and drive the lines it last set active back to inactive.
 */
void app_platform_clear_gpio_grants(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	line_grant[id] = 0U;
	if (app_svc_lock(&line_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
#if DT_NODE_EXISTS(LEDS_NODE)
	for (int i = 0; i < (int)LINE_COUNT; i++) {
		if ((line_active[id] & BIT(i)) != 0U) {
			(void)gpio_pin_set_dt(&lines[i], 0);
		}
	}
#endif
	line_active[id] = 0U;
	(void)app_svc_unlock(&line_lock);
}
static bool gpio_ready;

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

void app_platform_grant_gpio(int id, uint32_t line_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		line_grant[id] = line_mask;
	}
}

static void gpio_ready_init(void)
{
#if DT_NODE_EXISTS(LEDS_NODE)
	if (gpio_ready) {
		return;
	}
	for (int i = 0; i < (int)LINE_COUNT; i++) {
		if (gpio_is_ready_dt(&lines[i])) {
			gpio_pin_configure_dt(&lines[i], GPIO_OUTPUT_INACTIVE);
		}
	}
	gpio_ready = true;
#endif
}

#if DT_NODE_EXISTS(LEDS_NODE) && defined(CONFIG_APP_PLATFORM_EVENTS)
/* Bit N set means application N is watching this line. A line is configured as
 * an input and its interrupt enabled only while at least one application is
 * watching, so an unwatched line stays an output as before.
 */
static uint32_t line_watchers[LINE_COUNT];
static struct gpio_callback line_cb[LINE_COUNT];

static void line_changed(const struct device *dev, struct gpio_callback *cb,
			 uint32_t pins)
{
	ARG_UNUSED(dev);

	int line = (int)(cb - line_cb);

	if (line < 0 || line >= (int)LINE_COUNT) {
		return;
	}

	struct app_event evt = {
		.kind = APP_EVENT_GPIO,
		.line = (uint8_t)line,
		.value = (uint8_t)(gpio_pin_get_dt(&lines[line]) > 0 ? 1 : 0),
	};

	for (int id = 0; id < MAX_APPS; id++) {
		if ((line_watchers[line] & BIT(id)) != 0U) {
			app_events_post(id, &evt);
		}
	}
}
#endif

int z_impl_svc_gpio_count(void)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}
	return (int)LINE_COUNT;
}

int z_impl_svc_gpio_find(const char *name)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}
#if DT_NODE_EXISTS(LEDS_NODE)
	for (int i = 0; i < (int)LINE_COUNT; i++) {
		if (strcmp(line_name[i], name) == 0) {
			return i;
		}
	}
#else
	ARG_UNUSED(name);
#endif
	return -ENOENT;
}

int z_impl_svc_gpio_set(int line, int on)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || line < 0 || line >= (int)LINE_COUNT) {
		return -EINVAL;
	}
	if ((line_grant[id] & BIT(line)) == 0U) {
		return -EACCES;
	}
#if DT_NODE_EXISTS(LEDS_NODE)
	int rc;

	if (app_svc_lock(&line_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	gpio_ready_init();
	rc = gpio_pin_set_dt(&lines[line], on ? 1 : 0);
	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			line_active[i] &= ~BIT(line);
		}
		if (on != 0) {
			line_active[id] |= BIT(line);
		}
	}
	(void)app_svc_unlock(&line_lock);
	return rc;
#else
	ARG_UNUSED(on);
	return -ENODEV;
#endif
}

int z_impl_svc_gpio_watch(int line, int on)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || line < 0 || line >= (int)LINE_COUNT) {
		return -EINVAL;
	}
	if ((line_grant[id] & BIT(line)) == 0U) {
		return -EACCES;
	}
#if DT_NODE_EXISTS(LEDS_NODE) && defined(CONFIG_APP_PLATFORM_EVENTS)
	if (!gpio_is_ready_dt(&lines[line])) {
		return -ENODEV;
	}

	bool was_watched = (line_watchers[line] != 0U);

	if (on != 0) {
		line_watchers[line] |= BIT(id);
	} else {
		line_watchers[line] &= ~BIT(id);
	}

	bool watched = (line_watchers[line] != 0U);

	if (watched == was_watched) {
		return 0;
	}
	if (watched) {
		int rc = gpio_pin_configure_dt(&lines[line], GPIO_INPUT);

		if (rc != 0) {
			line_watchers[line] &= ~BIT(id);
			return rc;
		}
		gpio_init_callback(&line_cb[line], line_changed,
				   BIT(lines[line].pin));
		(void)gpio_add_callback(lines[line].port, &line_cb[line]);
		return gpio_pin_interrupt_configure_dt(&lines[line],
						       GPIO_INT_EDGE_BOTH);
	}

	(void)gpio_pin_interrupt_configure_dt(&lines[line], GPIO_INT_DISABLE);
	(void)gpio_remove_callback(lines[line].port, &line_cb[line]);
	return gpio_pin_configure_dt(&lines[line], GPIO_OUTPUT_INACTIVE);
#else
	ARG_UNUSED(on);
	return -ENOTSUP;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_gpio_watch(int line, int on)
{
	return z_impl_svc_gpio_watch(line, on);
}
#include <zephyr/syscalls/svc_gpio_watch_mrsh.c>

static inline int z_vrfy_svc_gpio_count(void)
{
	return z_impl_svc_gpio_count();
}
#include <zephyr/syscalls/svc_gpio_count_mrsh.c>

static inline int z_vrfy_svc_gpio_find(const char *name)
{
	char buf[IO_NAME_MAX];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_gpio_find(buf);
}
#include <zephyr/syscalls/svc_gpio_find_mrsh.c>

static inline int z_vrfy_svc_gpio_set(int line, int on)
{
	return z_impl_svc_gpio_set(line, on);
}
#include <zephyr/syscalls/svc_gpio_set_mrsh.c>
#endif
