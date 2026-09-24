/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Per-app resource inspection. Three workers with different stack and CPU
 * profiles run alongside an inspector app that holds the inspect capability. The
 * inspector prints a top-style table of every group's stack high-water mark and
 * CPU share, reading the others only through a capability-gated broker call - so
 * an app cannot observe its neighbours unless the platform granted it the right.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern void app_spinner(void *arg);
extern void app_sleeper(void *arg);
extern void app_deep(void *arg);
extern void app_inspector(void *arg);

int main(void)
{
	printk("\n*** app platform: per-app resource inspection (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	const struct app_def apps[] = {
		{"spinner", APP_BUILTIN, app_spinner, APP_CAP_LOG, false},
		{"sleeper", APP_BUILTIN, app_sleeper, APP_CAP_LOG, false},
		{"deep", APP_BUILTIN, app_deep, APP_CAP_LOG, false},
		{"inspector", APP_BUILTIN, app_inspector, APP_CAP_LOG | APP_CAP_INSPECT, false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(apps); i++) {
		app_platform_launch(&apps[i]);
	}

	k_msleep(6000);

	printk("\n*** done: inspector reported per-app stack and cpu use ***\n");
	return 0;
}
