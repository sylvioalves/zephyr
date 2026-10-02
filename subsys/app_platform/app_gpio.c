/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The shared GPIO service. The platform owns a set of lines taken from the
 * "gpios" property of the zephyr,app-platform devicetree node. An application
 * that holds APP_CAP_GPIO may drive only the lines it was granted (see
 * app_platform_grant_gpio), so the capability admits the service and a
 * per-line mask scopes it to specific pins - the second-level grant pattern.
 * A line an application last drove active is driven inactive when that
 * application is reclaimed.
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

#define HAS_LINES DT_NODE_HAS_PROP(APP_PLATFORM_NODE, gpios)

#if HAS_LINES
#define GPIO_SPEC(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),
#define GPIO_NAME(node_id, prop, idx)                                                     \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, line_names),                                 \
		    (DT_PROP_BY_IDX(node_id, line_names, idx)), ("")),

static const struct gpio_dt_spec lines[] = {
	DT_FOREACH_PROP_ELEM(APP_PLATFORM_NODE, gpios, GPIO_SPEC)
};
#define LINE_COUNT ARRAY_SIZE(lines)

static const char *const line_name[] = {
	DT_FOREACH_PROP_ELEM(APP_PLATFORM_NODE, gpios, GPIO_NAME)
};
#else
#define LINE_COUNT 0
#endif

#define IO_NAME_MAX 24

static uint32_t line_grant[MAX_APPS];
/* Bit N set means this application last drove line N active. */
static uint32_t line_active[MAX_APPS];
/* Bit N set means this application reconfigured line N. */
static uint32_t line_configured[MAX_APPS];
static SVC_LOCK_DEFINE(line_lock);

#if HAS_LINES
/* Bias and drive flags an application set on each line, kept when the line
 * turns into a watched input.
 */
static gpio_flags_t line_extra[LINE_COUNT];
#endif

#if HAS_LINES && defined(CONFIG_APP_PLATFORM_EVENTS)
/* Bit N set means application N is watching this line. A line is configured as
 * an input and its interrupt enabled only while at least one application is
 * watching, so an unwatched line stays an output.
 */
static uint32_t line_watchers[LINE_COUNT];
static struct gpio_callback line_cb[LINE_COUNT];
/* Per application, the lines whose rising or falling edges it wants. */
static uint32_t watch_rise[MAX_APPS];
static uint32_t watch_fall[MAX_APPS];
static uint16_t watch_debounce[MAX_APPS][LINE_COUNT];
static uint32_t watch_last[MAX_APPS][LINE_COUNT];

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
	uint32_t now = k_uptime_get_32();

	for (int id = 0; id < MAX_APPS; id++) {
		uint32_t want = (evt.value != 0U) ? watch_rise[id] : watch_fall[id];

		if ((line_watchers[line] & BIT(id)) == 0U || (want & BIT(line)) == 0U) {
			continue;
		}
		if (watch_debounce[id][line] != 0U &&
		    now - watch_last[id][line] < watch_debounce[id][line]) {
			continue;
		}
		watch_last[id][line] = now;
		app_events_post(id, &evt);
	}
}

/* Add or drop one application's watch on a line, and switch the line between
 * an output and an interrupt-enabled input as its first watcher arrives or its
 * last one leaves. Called with line_lock held.
 */
static int line_watch_set(int line, int id, bool on)
{
	bool was_watched = (line_watchers[line] != 0U);

	if (on) {
		line_watchers[line] |= BIT(id);
	} else {
		line_watchers[line] &= ~BIT(id);
		watch_rise[id] &= ~BIT(line);
		watch_fall[id] &= ~BIT(line);
		watch_debounce[id][line] = 0U;
	}

	bool watched = (line_watchers[line] != 0U);

	if (watched == was_watched) {
		return 0;
	}
	if (watched) {
		int rc = gpio_pin_configure_dt(&lines[line], GPIO_INPUT | line_extra[line]);

		if (rc != 0) {
			line_watchers[line] &= ~BIT(id);
			return rc;
		}
		gpio_init_callback(&line_cb[line], line_changed,
				   BIT(lines[line].pin));
		(void)gpio_add_callback(lines[line].port, &line_cb[line]);
		rc = gpio_pin_interrupt_configure_dt(&lines[line], GPIO_INT_EDGE_BOTH);
		if (rc != 0) {
			line_watchers[line] &= ~BIT(id);
			(void)gpio_remove_callback(lines[line].port, &line_cb[line]);
			(void)gpio_pin_configure_dt(&lines[line], GPIO_OUTPUT_INACTIVE);
		}
		return rc;
	}

	(void)gpio_pin_interrupt_configure_dt(&lines[line], GPIO_INT_DISABLE);
	(void)gpio_remove_callback(lines[line].port, &line_cb[line]);
	return gpio_pin_configure_dt(&lines[line], GPIO_OUTPUT_INACTIVE);
}

/* Watch a line for the edges in @p edge, or stop watching it when @p edge is
 * negative.
 */
static int line_watch_edges(int line, int id, int edge, uint16_t debounce_ms)
{
	int rc;

	if (app_service_lock(&line_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	if (edge < 0) {
		rc = line_watch_set(line, id, false);
	} else {
		WRITE_BIT(watch_rise[id], line, edge != APP_GPIO_EDGE_FALLING);
		WRITE_BIT(watch_fall[id], line, edge != APP_GPIO_EDGE_RISING);
		watch_debounce[id][line] = debounce_ms;
		watch_last[id][line] = k_uptime_get_32() - debounce_ms;
		rc = line_watch_set(line, id, true);
	}
	(void)app_service_unlock(&line_lock);
	return rc;
}
#endif

#if HAS_LINES
static bool line_is_watched(int line)
{
#ifdef CONFIG_APP_PLATFORM_EVENTS
	return line_watchers[line] != 0U;
#else
	ARG_UNUSED(line);
	return false;
#endif
}
#endif

/* Drop every line grant of a slot that is being handed to a new application
 * and drive the lines it last set active back to inactive.
 */
void app_platform_clear_gpio_grants(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	line_grant[id] = 0U;
	if (app_service_lock(&line_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
#if HAS_LINES
	for (int i = 0; i < (int)LINE_COUNT; i++) {
#ifdef CONFIG_APP_PLATFORM_EVENTS
		if ((line_watchers[i] & BIT(id)) != 0U) {
			(void)line_watch_set(i, id, false);
		}
#endif
		if ((line_configured[id] & BIT(i)) != 0U) {
			line_extra[i] = 0U;
			if (!line_is_watched(i)) {
				(void)gpio_pin_configure_dt(&lines[i], GPIO_OUTPUT_INACTIVE);
			}
		}
		if ((line_active[id] & BIT(i)) != 0U) {
			(void)gpio_pin_set_dt(&lines[i], 0);
		}
	}
#endif
	line_active[id] = 0U;
	line_configured[id] = 0U;
	(void)app_service_unlock(&line_lock);
}

void app_platform_grant_gpio(int id, uint32_t line_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		line_grant[id] = line_mask;
	}
}

#if HAS_LINES
static bool gpio_ready;

static void gpio_ready_init(void)
{
	if (gpio_ready) {
		return;
	}
	for (int i = 0; i < (int)LINE_COUNT; i++) {
		if (gpio_is_ready_dt(&lines[i])) {
			gpio_pin_configure_dt(&lines[i], GPIO_OUTPUT_INACTIVE);
		}
	}
	gpio_ready = true;
}
#endif

int z_impl_app_svc_gpio_count(void)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}
	return (int)LINE_COUNT;
}

int z_impl_app_svc_gpio_find(const char *name)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}
#if HAS_LINES
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

int z_impl_app_svc_gpio_set(int line, int on)
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
#if HAS_LINES
	int rc;

	if (app_service_lock(&line_lock, SVC_LOCK_TIMEOUT) != 0) {
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
	(void)app_service_unlock(&line_lock);
	return rc;
#else
	ARG_UNUSED(on);
	return -ENODEV;
#endif
}

#if HAS_LINES
/* Drive the lines in @p mask, one driver call per port. */
static int lines_set_mask(uint32_t mask, uint32_t values)
{
	uint32_t left = mask;

	while (left != 0U) {
		const struct device *port = lines[find_lsb_set(left) - 1].port;
		gpio_port_pins_t pins = 0U;
		gpio_port_value_t levels = 0U;
		int rc;

		for (int i = 0; i < (int)LINE_COUNT; i++) {
			if ((left & BIT(i)) == 0U || lines[i].port != port) {
				continue;
			}
			bool high = ((values & BIT(i)) != 0U) !=
				    ((lines[i].dt_flags & GPIO_ACTIVE_LOW) != 0U);

			pins |= BIT(lines[i].pin);
			if (high) {
				levels |= BIT(lines[i].pin);
			}
			left &= ~BIT(i);
		}
		rc = gpio_port_set_masked_raw(port, pins, levels);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}
#endif

int z_impl_app_svc_gpio_set_mask(uint32_t mask, uint32_t values)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	if ((mask & ~(uint32_t)BIT64_MASK(LINE_COUNT)) != 0U) {
		return -EINVAL;
	}
	if ((mask & ~line_grant[id]) != 0U) {
		return -EACCES;
	}
	if (mask == 0U) {
		return 0;
	}
#if HAS_LINES
	int rc;

	if (app_service_lock(&line_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	gpio_ready_init();
	rc = lines_set_mask(mask, values);
	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			line_active[i] &= ~mask;
		}
		line_active[id] |= mask & values;
	}
	(void)app_service_unlock(&line_lock);
	return rc;
#else
	ARG_UNUSED(values);
	return -ENODEV;
#endif
}

int z_impl_app_svc_gpio_watch(int line, int on)
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
#if HAS_LINES && defined(CONFIG_APP_PLATFORM_EVENTS)
	if (!gpio_is_ready_dt(&lines[line])) {
		return -ENODEV;
	}

	return line_watch_edges(line, id, (on != 0) ? APP_GPIO_EDGE_BOTH : -1, 0U);
#else
	ARG_UNUSED(on);
	return -ENOTSUP;
#endif
}

int z_impl_app_svc_gpio_watch_edge(int line, uint8_t edge, uint16_t debounce_ms)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || line < 0 || line >= (int)LINE_COUNT ||
	    edge > APP_GPIO_EDGE_FALLING) {
		return -EINVAL;
	}
	if ((line_grant[id] & BIT(line)) == 0U) {
		return -EACCES;
	}
#if HAS_LINES && defined(CONFIG_APP_PLATFORM_EVENTS)
	if (!gpio_is_ready_dt(&lines[line])) {
		return -ENODEV;
	}
	return line_watch_edges(line, id, edge, debounce_ms);
#else
	ARG_UNUSED(debounce_ms);
	return -ENOTSUP;
#endif
}

int z_impl_app_svc_gpio_get(int line)
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
#if HAS_LINES
	if (!gpio_is_ready_dt(&lines[line])) {
		return -ENODEV;
	}
	return gpio_pin_get_dt(&lines[line]);
#else
	return -ENODEV;
#endif
}

int z_impl_app_svc_gpio_get_mask(uint32_t mask, uint32_t *values)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	if ((mask & ~(uint32_t)BIT64_MASK(LINE_COUNT)) != 0U) {
		return -EINVAL;
	}
	if ((mask & ~line_grant[id]) != 0U) {
		return -EACCES;
	}
#if HAS_LINES
	uint32_t got = 0U;
	uint32_t left = mask;

	while (left != 0U) {
		const struct device *port = lines[find_lsb_set(left) - 1].port;
		gpio_port_value_t raw;
		int rc = gpio_port_get_raw(port, &raw);

		if (rc != 0) {
			return rc;
		}
		for (int i = 0; i < (int)LINE_COUNT; i++) {
			if ((left & BIT(i)) == 0U || lines[i].port != port) {
				continue;
			}
			bool high = (raw & BIT(lines[i].pin)) != 0U;

			if (high != ((lines[i].dt_flags & GPIO_ACTIVE_LOW) != 0U)) {
				got |= BIT(i);
			}
			left &= ~BIT(i);
		}
	}
	*values = got;
	return 0;
#else
	ARG_UNUSED(values);
	return -ENODEV;
#endif
}

int z_impl_app_svc_gpio_configure(int line, uint32_t flags)
{
	if (!app_has_cap(APP_CAP_GPIO)) {
		return -EPERM;
	}

	int id = app_platform_id_current();
	uint32_t dir = flags & (APP_GPIO_INPUT | APP_GPIO_OUTPUT);
	const uint32_t known = APP_GPIO_INPUT | APP_GPIO_OUTPUT | APP_GPIO_PULL_UP |
			       APP_GPIO_PULL_DOWN | APP_GPIO_OPEN_DRAIN;

	if (id < 0 || id >= MAX_APPS || line < 0 || line >= (int)LINE_COUNT) {
		return -EINVAL;
	}
	if ((flags & ~known) != 0U || (dir != APP_GPIO_INPUT && dir != APP_GPIO_OUTPUT) ||
	    ((flags & APP_GPIO_PULL_UP) != 0U && (flags & APP_GPIO_PULL_DOWN) != 0U) ||
	    ((flags & APP_GPIO_OPEN_DRAIN) != 0U && dir != APP_GPIO_OUTPUT)) {
		return -EINVAL;
	}
	if ((line_grant[id] & BIT(line)) == 0U) {
		return -EACCES;
	}
#if HAS_LINES
	gpio_flags_t extra = 0U;
	int rc;

	if ((flags & APP_GPIO_PULL_UP) != 0U) {
		extra |= GPIO_PULL_UP;
	}
	if ((flags & APP_GPIO_PULL_DOWN) != 0U) {
		extra |= GPIO_PULL_DOWN;
	}
	if ((flags & APP_GPIO_OPEN_DRAIN) != 0U) {
		extra |= GPIO_OPEN_DRAIN;
	}
	if (app_service_lock(&line_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	gpio_ready_init();
	if (dir == APP_GPIO_OUTPUT && line_is_watched(line)) {
		rc = -EBUSY;
	} else if (dir == APP_GPIO_OUTPUT) {
		rc = gpio_pin_configure_dt(&lines[line], GPIO_OUTPUT_INACTIVE | extra);
	} else {
		rc = gpio_pin_configure_dt(&lines[line], GPIO_INPUT | extra);
#ifdef CONFIG_APP_PLATFORM_EVENTS
		if (rc == 0 && line_is_watched(line)) {
			rc = gpio_pin_interrupt_configure_dt(&lines[line], GPIO_INT_EDGE_BOTH);
		}
#endif
	}
	if (rc == 0) {
		line_extra[line] = (dir == APP_GPIO_INPUT) ? extra : 0U;
		line_configured[id] |= BIT(line);
		for (int i = 0; i < MAX_APPS; i++) {
			line_active[i] &= ~BIT(line);
		}
	}
	(void)app_service_unlock(&line_lock);
	return rc;
#else
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_gpio_watch_edge(int line, uint8_t edge, uint16_t debounce_ms)
{
	return z_impl_app_svc_gpio_watch_edge(line, edge, debounce_ms);
}
#include <zephyr/syscalls/app_svc_gpio_watch_edge_mrsh.c>

static inline int z_vrfy_app_svc_gpio_get(int line)
{
	return z_impl_app_svc_gpio_get(line);
}
#include <zephyr/syscalls/app_svc_gpio_get_mrsh.c>

static inline int z_vrfy_app_svc_gpio_get_mask(uint32_t mask, uint32_t *values)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(values, sizeof(*values)));
	return z_impl_app_svc_gpio_get_mask(mask, values);
}
#include <zephyr/syscalls/app_svc_gpio_get_mask_mrsh.c>

static inline int z_vrfy_app_svc_gpio_configure(int line, uint32_t flags)
{
	return z_impl_app_svc_gpio_configure(line, flags);
}
#include <zephyr/syscalls/app_svc_gpio_configure_mrsh.c>

static inline int z_vrfy_app_svc_gpio_watch(int line, int on)
{
	return z_impl_app_svc_gpio_watch(line, on);
}
#include <zephyr/syscalls/app_svc_gpio_watch_mrsh.c>

static inline int z_vrfy_app_svc_gpio_count(void)
{
	return z_impl_app_svc_gpio_count();
}
#include <zephyr/syscalls/app_svc_gpio_count_mrsh.c>

static inline int z_vrfy_app_svc_gpio_find(const char *name)
{
	char buf[IO_NAME_MAX];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_app_svc_gpio_find(buf);
}
#include <zephyr/syscalls/app_svc_gpio_find_mrsh.c>

static inline int z_vrfy_app_svc_gpio_set(int line, int on)
{
	return z_impl_app_svc_gpio_set(line, on);
}
#include <zephyr/syscalls/app_svc_gpio_set_mrsh.c>

static inline int z_vrfy_app_svc_gpio_set_mask(uint32_t mask, uint32_t values)
{
	return z_impl_app_svc_gpio_set_mask(mask, values);
}
#include <zephyr/syscalls/app_svc_gpio_set_mask_mrsh.c>
#endif
