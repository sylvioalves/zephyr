/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * External-RAM mapping demo. A loadable app is verified and launched isolated,
 * granted the PSRAM capability, and maps a region of external RAM into its own
 * domain to use directly. The platform frees the region when the app exits.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t psram_app[] __aligned(8) = {
#include "psram.inc"
};
static const uint8_t psram_hash[32] = {
#include "psram.sha256.inc"
};

int main(void)
{
	printk("\n*** external RAM mapping demo (C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("psram", psram_app, ARRAY_SIZE(psram_app),
					psram_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("install failed: %d\n", rc);
		return 0;
	}

	app_platform_launch_llext("psram", ext, (app_entry_t)entry, NULL,
				  APP_CAP_LOG | APP_CAP_TIME | APP_CAP_PSRAM);

	k_msleep(8000);

	printk("\n*** done: app used mapped external RAM directly ***\n");
	return 0;
}
