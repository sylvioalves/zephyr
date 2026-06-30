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
 * the second-level grant pattern. Driving a line is stateless beyond that check.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

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

static uint32_t line_grant[MAX_APPS];
static bool gpio_ready;

static inline bool app_has_cap(uint32_t mask)
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
		return -EPERM;
	}
#if DT_NODE_EXISTS(LEDS_NODE)
	gpio_ready_init();
	return gpio_pin_set_dt(&lines[line], on ? 1 : 0);
#else
	ARG_UNUSED(on);
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE
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
