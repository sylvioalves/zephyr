/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Service-breadth probe. A loadable app is verified, loaded and run isolated;
 * it calls a spread of brokered services covering every syscall boundary shape
 * and reports which work from inside an llext module. This maps how much of the
 * service surface is already reachable from downloadable apps.
 */

#include "svc.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t probe_app[] = {
#include "probe_app.inc"
};
static const uint8_t probe_app_hash[32] = {
#include "probe_app.sha256.inc"
};

int main(void)
{
	printk("\n*** service-breadth probe from a loadable app (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("probe", probe_app, ARRAY_SIZE(probe_app),
					probe_app_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("load failed: %d\n", rc);
		return 0;
	}

	app_platform_launch_llext("probe", ext, (app_entry_t)entry, 0);

	k_msleep(4000);

	printk("\n*** done: probed the service surface from an llext app ***\n");
	return 0;
}
