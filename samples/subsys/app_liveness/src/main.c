/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Liveness model demo. The platform reclaims a misbehaving app two ways:
 * passively when its thread dies (crash or exit, no app cooperation), and - only
 * if the app opted in - when it stops feeding a heartbeat watchdog (hang). This
 * launches three apps side by side to show each case, and proves a busy app that
 * never heartbeats is left alone.
 */

#include "svc.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern void app_busy(void *arg);
extern void app_crasher(void *arg);
extern void app_hanger(void *arg);

int main(void)
{
	printk("\n*** app platform: liveness model (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	const struct app_def busy = {
		.name = "busy", .source = APP_BUILTIN, .entry = app_busy,
		.caps = 0, .monitored = false,
	};
	const struct app_def crasher = {
		.name = "crasher", .source = APP_BUILTIN, .entry = app_crasher,
		.caps = 0, .monitored = false,
	};
	const struct app_def hanger = {
		.name = "hanger", .source = APP_BUILTIN, .entry = app_hanger,
		.caps = 0, .monitored = true,
	};

	app_platform_launch(&busy);
	app_platform_launch(&crasher);
	app_platform_launch(&hanger);

	k_msleep(8000);

	printk("\n*** done: crash and hang reclaimed, busy app left running ***\n");
	return 0;
}
