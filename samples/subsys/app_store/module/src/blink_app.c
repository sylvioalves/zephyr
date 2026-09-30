/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable "app" module. In a full system this is fetched over the
 * network, its signed manifest verified, stored, and installed at runtime. It
 * is built as an llext extension and embedded for the demo, but the install
 * path is identical: verify the manifest, bind the module, launch it as an
 * isolated user-mode application. The entry takes the isolated-app signature so
 * the platform runs it in its own memory domain, reaching the system only
 * through service syscalls.
 */

#include <zephyr/app_platform/app_sdk.h>

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	for (int i = 0; i < 5; i++) {
		app_svc_log("store app: running from a verified install");
		app_sleep_ms(500);
	}
	app_svc_log("store app: done");
}
EXPORT_SYMBOL(app_main);
