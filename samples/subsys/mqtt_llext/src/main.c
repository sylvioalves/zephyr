/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable MQTT app over the shared service library. The platform owns the
 * Wi-Fi, network and MQTT stacks and the die-temperature sensor; the reporter
 * arrives as a verified llext module, runs isolated, and reaches all of it
 * through capability-gated app_svc_ calls. The module is embedded here to stand in
 * for a downloaded release.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t temp_app[] __aligned(8) = {
#include "temp_app.inc"
};
static const uint8_t temp_app_hash[32] = {
#include "temp_app.sha256.inc"
};

int main(void)
{
	printk("\n*** downloadable MQTT die-temp app over shared services (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("temp-app", temp_app, ARRAY_SIZE(temp_app),
					temp_app_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("install failed: %d\n", rc);
		return 0;
	}

	uint32_t caps = APP_CAP_LOG | APP_CAP_SENSOR | APP_CAP_NET;
	int id = app_platform_launch_llext("temp-app", ext, (app_entry_t)entry, NULL, caps);

	printk("temp-app launched as isolated group %d\n", id);

	k_msleep(40000);

	printk("\n*** done: a downloadable app published die temp over MQTT ***\n");
	return 0;
}
