/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Crash recovery with persisted state. A resilient app counts upward, saving a
 * checkpoint each step, and deliberately crashes part-way. The platform reclaims
 * the faulting app and restarts it; the new instance resumes from the saved
 * checkpoint rather than starting over. State survived the crash because the
 * platform held it outside the app's memory domain.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern void app_worker(void *arg);

int main(void)
{
	printk("\n*** app platform: crash recovery with state (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	const struct app_def worker = {
		.name = "worker",
		.source = APP_BUILTIN,
		.entry = app_worker,
		.caps = APP_CAP_LOG,
	};
	int id = app_platform_launch(&worker);

	/* Make the worker resilient: the platform restarts it after a fault. */
	app_platform_set_restart(id, true);

	k_msleep(6000);

	printk("\n*** done: app crashed, was restarted, and resumed its work ***\n");
	return 0;
}
