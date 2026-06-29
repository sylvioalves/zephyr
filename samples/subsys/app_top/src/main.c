/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Launch one built-in and one loadable application, then leave a shell up so
 * both can be inspected and controlled live. The built-in app is compiled into
 * the firmware; the loadable app is a verified module loaded at runtime. Both
 * run as isolated user-mode threads looping with a one-second period. Run
 * "app status" on the console to compare them (the built-in shows a zero module
 * footprint, the loadable shows its loaded size) and "app stop/start/restart/
 * remove <id>" to control them.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t app_bytes[] = {
#include "loop_app.inc"
};
static const uint8_t app_hash[32] = {
#include "loop_app.sha256.inc"
};

#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME)

/* Built-in app: runs as a user-mode thread, reaching the system only through
 * service syscalls, the same as a loadable app.
 */
static void builtin_hello(void *arg)
{
	ARG_UNUSED(arg);

	for (;;) {
		svc_log("builtin: hello from a compiled-in app");
		k_msleep(1000);
	}
}

int main(void)
{
	printk("\n*** built-in + loadable app demo (run: app status) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	/* 1. A built-in application, compiled into the firmware. */
	const struct app_def builtin = {
		.name = "builtin0",
		.source = APP_BUILTIN,
		.entry = builtin_hello,
		.caps = APP_CAPS,
		.monitored = false,
	};

	if (app_platform_launch(&builtin) < 0) {
		printk("built-in launch failed\n");
	}

	/* 2. A loadable application, verified and loaded at runtime. */
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("loadable0", app_bytes,
					ARRAY_SIZE(app_bytes), app_hash,
					"app_main", &ext, &entry);

	if (rc == 0) {
		app_platform_launch_llext("loadable0", ext, (app_entry_t)entry,
					  APP_CAPS);
	} else {
		printk("loadable load failed: %d\n", rc);
	}

	printk("*** ready: built-in (id 0) + loadable (id 1); try app status ***\n");
	return 0;
}
