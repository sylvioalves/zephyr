/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Load a verified RGB LED application, grant it pixel 0 and wait for it to
 * finish cycling colors.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define RGB_APP_WAIT_MS 30000

static const uint8_t rgb_app[] __aligned(8) = {
#include "rgb_app.inc"
};
static const uint8_t rgb_app_hash[32] = {
#include "rgb_app.sha256.inc"
};

int main(void)
{
	struct app_platform_status st;
	struct llext *ext = NULL;
	void *entry = NULL;
	int id;
	int rc;

	printk("\n*** app_rgb: RGB LED through a platform service ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	rc = app_loader_verify_load("rgb", rgb_app, ARRAY_SIZE(rgb_app), rgb_app_hash,
				    "app_main", &ext, &entry);
	if (rc != 0) {
		printk("load failed: %d\n", rc);
		return 0;
	}

	/* The only application slot is 0, so the grant is in place before launch. */
	app_platform_grant_rgb(0, BIT(0));

	id = app_platform_launch_llext("rgb", ext, (app_entry_t)entry, NULL,
				       APP_CAP_LOG | APP_CAP_RGB);
	if (id < 0) {
		printk("launch failed: %d\n", id);
		return 0;
	}

	for (int i = 0; i < RGB_APP_WAIT_MS / 100; i++) {
		if (app_platform_status(id, &st) != 0 || !st.active) {
			break;
		}
		k_msleep(100);
	}

	if (app_platform_status(id, &st) == 0 && st.active) {
		printk("\n*** done: rgb app still running after %d ms ***\n", RGB_APP_WAIT_MS);
		return 0;
	}

	printk("\n*** done: rgb app cycled pixel 0 and exited ***\n");
	return 0;
}
