/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable platform app, built as an llext and loaded at runtime. It runs
 * as an isolated user-mode thread in its own memory domain and reaches the
 * platform through the same service syscalls a built-in app uses. The platform
 * verifies it, grants it a capability set, supervises it, and can reclaim it.
 */

#include "svc.h"

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

void app_main(void *arg)
{
	ARG_UNUSED(arg);
	int on = 0;

	svc_log("installed-llext: hello from a runtime-loaded app");

	for (int i = 0; i < 8; i++) {
		int rc = svc_led_set(on);

		on = !on;
		if (i == 0 && rc != 0) {
			svc_log("installed-llext: led denied (no CAP_GPIO)");
		}
		svc_log("installed-llext: blink + heartbeat");
		svc_heartbeat();
		k_msleep(500);
	}

	svc_log("installed-llext: done");
}
EXPORT_SYMBOL(app_main);
