/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t ble_llext_app[] = {
#include "ble_llext_app.inc"
};
static const uint8_t ble_llext_app_hash[] = {
#include "ble_llext_app.sha256.inc"
};

/* The service the application is allowed to serve. It must match the UUID the
 * module passes to app_svc_ble_serve_start().
 */
static const uint8_t svc_uuid[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x44, 0x44, 0xa0, 0xc3, 0xb2, 0xa1,
};

int main(void)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc;

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	rc = app_loader_verify_load("ble_llext", ble_llext_app, ARRAY_SIZE(ble_llext_app),
				    ble_llext_app_hash, "app_main", &ext, &entry);
	if (rc != 0) {
		printk("module load failed (%d)\n", rc);
		return 0;
	}

	rc = app_platform_launch_llext("ble_llext", ext, (app_entry_t)entry, NULL,
				       APP_CAP_LOG | APP_CAP_BLE | APP_CAP_INPUT);
	if (rc < 0) {
		printk("launch failed (%d)\n", rc);
		return 0;
	}

	app_platform_grant_ble_gatt(rc, svc_uuid, sizeof(svc_uuid));
	printk("launched loadable peripheral app %d\n", rc);
	return 0;
}
