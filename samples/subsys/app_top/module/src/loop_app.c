/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal loadable application that prints once per second, matching the
 * built-in app's cadence so the two can be compared directly. It uses only
 * service syscalls, so it has no libc or supervisor-symbol dependencies to
 * resolve at load time.
 */

#include <zephyr/app_platform/app_services.h>

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	for (;;) {
		svc_log("loadable: hello from a loaded module");
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
