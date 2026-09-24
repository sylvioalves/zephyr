/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A versioned downloadable app, built as an llext. The same source is built
 * twice (APP_VERSION 1 and 2) to stand in for two releases of one app. Each
 * version logs its own banner, so the hot-update is visible at runtime.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

#ifndef APP_VERSION
#define APP_VERSION 0
#endif

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	if (APP_VERSION == 1) {
		svc_log("app v1: running (the old release)");
	} else {
		svc_log("app v2: running (the updated release, new behavior)");
	}

	for (int i = 0; i < 20; i++) {
		if (APP_VERSION == 1) {
			svc_log("app v1: tick");
		} else {
			svc_log("app v2: tick (now with extra logging)");
		}
		svc_heartbeat();
		k_msleep(500);
	}
}
EXPORT_SYMBOL(app_main);
