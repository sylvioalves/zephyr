/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal loadable application that prints once per second, matching the
 * built-in app's cadence so the two can be compared directly. It calls only
 * the functions of app_sdk.h, so it resolves nothing but the platform's
 * exported functions at load time.
 */

#include <zephyr/app_platform/app_sdk.h>

#include <zephyr/sys/util.h>

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	for (;;) {
		app_svc_log("loadable: hello from a loaded module");
		app_sleep_ms(1000);
	}
}
EXPORT_SYMBOL(app_main);
