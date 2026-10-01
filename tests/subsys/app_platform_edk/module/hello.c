/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Sleeps and reads the uptime through the kernel functions and a service, and
 * returns once both measured the sleep it asked for. It retries otherwise.
 */

#include <zephyr/app_platform/app_sdk.h>

#define SLEEP_MS 50

static bool measured(int64_t ms)
{
	return ms >= SLEEP_MS && ms < 10 * SLEEP_MS;
}

static bool check(void)
{
	int64_t start = app_uptime_get();
	uint32_t svc_start = app_svc_uptime_ms();

	if (app_sleep_ms(SLEEP_MS) != 0) {
		return false;
	}
	return measured(app_uptime_get() - start) &&
	       measured((int64_t)(uint32_t)(app_svc_uptime_ms() - svc_start));
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	while (!check()) {
		app_svc_log("edk_hello: sleep not measured, retrying");
		app_sleep_ms(1000);
	}
	app_svc_log("edk_hello: module measured the sleep");
}
EXPORT_SYMBOL(app_main);
