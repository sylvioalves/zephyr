/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Hot-updating a running app. Version 1 of an app is verified, loaded, and
 * launched as an isolated user-mode group. While it runs, a new release
 * (version 2) "arrives": the platform verifies it, stops the running v1, and
 * launches v2 in its place. The app is replaced live, without rebooting and
 * without disturbing the rest of the system. Both versions are embedded here to
 * stand in for two downloaded releases.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t app_v1[] __aligned(8) = {
#include "app_v1.inc"
};
static const uint8_t app_v1_hash[32] = {
#include "app_v1.sha256.inc"
};

static const uint8_t app_v2[] __aligned(8) = {
#include "app_v2.inc"
};
static const uint8_t app_v2_hash[32] = {
#include "app_v2.sha256.inc"
};

/* Verify, load and launch a release; returns the running group id. */
static int install_version(const char *name, const uint8_t *buf, size_t len,
			   const uint8_t hash[32])
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load(name, buf, len, hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("update: install of %s failed: %d\n", name, rc);
		return rc;
	}
	return app_platform_launch_llext(name, ext, (app_entry_t)entry, NULL, APP_CAP_LOG);
}

int main(void)
{
	printk("\n*** app platform: live app hot-update (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	printk("\ninstalling app v1...\n");
	int id = install_version("app-v1", app_v1, ARRAY_SIZE(app_v1), app_v1_hash);

	if (id < 0) {
		return 0;
	}

	/* Let v1 run for a bit. */
	k_msleep(2500);

	printk("\na new release arrived - hot-updating to v2...\n");
	printk("stopping running v1 (group %d)\n", id);
	app_platform_stop(id);

	int id2 = install_version("app-v2", app_v2, ARRAY_SIZE(app_v2), app_v2_hash);

	printk("v2 launched as group %d - the app was updated live\n", id2);

	k_msleep(3000);

	printk("\n*** done: app updated at runtime, no reboot ***\n");
	return 0;
}
