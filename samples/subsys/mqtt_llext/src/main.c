/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable MQTT app. The supervisor brings up Wi-Fi once and owns the
 * network and MQTT stacks; the die-temperature reporter arrives as a verified
 * llext module, is loaded into its own memory domain, and runs as an isolated
 * user-mode app that subscribes to a command topic and publishes the die
 * temperature every ten seconds through brokered svc_ calls. The module is
 * embedded here to stand in for a downloaded release.
 */

#include "svc.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern int net_link_up(void);

static const uint8_t temp_app[] = {
#include "temp_app.inc"
};
static const uint8_t temp_app_hash[32] = {
#include "temp_app.sha256.inc"
};

int main(void)
{
	printk("\n*** downloadable MQTT die-temp app (llext, C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	if (net_link_up() != 0) {
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

	int id = app_platform_launch_llext("temp-app", ext, (app_entry_t)entry, 0);

	printk("temp-app launched as isolated group %d\n", id);

	k_msleep(35000);

	printk("\n*** done: a downloadable app published die temp over MQTT ***\n");
	return 0;
}
