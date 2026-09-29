/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PLATFORM_TEST_LAUNCH_H_
#define APP_PLATFORM_TEST_LAUNCH_H_

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>

#define SETTLE_MS 20

static inline void settle_then_run(void *entry)
{
	k_msleep(SETTLE_MS);
	((app_entry_t)entry)(NULL);
}

/* On SMP, delay an application's own code until the test has set up its domain. */
static inline int launch_app(const struct app_def *def)
{
	struct app_def d = *def;

	if (IS_ENABLED(CONFIG_SMP)) {
		d.arg = (void *)d.entry;
		d.entry = settle_then_run;
	}
	return app_platform_launch(&d);
}

#endif /* APP_PLATFORM_TEST_LAUNCH_H_ */
