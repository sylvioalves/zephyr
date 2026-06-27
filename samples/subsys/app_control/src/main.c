/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Runtime lifecycle control. A manager app - itself an isolated app, but holding
 * the control capability - starts and stops other apps while the system runs.
 * The platform is a controllable OS: launching and reclaiming apps is driven by
 * an app through brokered, capability-gated calls, not hardcoded at boot.
 */

#include "app_caps.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern void app_manager(void *arg);

int main(void)
{
	printk("\n*** app platform: runtime lifecycle control (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	/* The manager app holds the control capability; the workers do not. */
	const struct app_def manager = {
		.name = "manager", .source = APP_BUILTIN,
		.entry = app_manager, .caps = CAP_CONTROL,
	};
	app_platform_launch(&manager);

	k_msleep(5000);

	printk("\n*** done: an app started and stopped other apps at runtime ***\n");
	return 0;
}
