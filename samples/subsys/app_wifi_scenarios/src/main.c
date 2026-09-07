/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Wi-Fi scenarios the other samples do not cover: a TCP round trip over the
 * shared link, and losing the access point while an application is running.
 *
 * Everything runs inside the loadable application, because the services are
 * reachable only from an application thread that holds the capabilities; the
 * supervisor here just loads it. Power the access point off and on while the
 * application is watching to see the platform notice the loss and recover.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_NET | APP_CAP_SOCKET)

static const uint8_t net_app[] = {
#include "net_app.inc"
};
static const uint8_t net_hash[32] = {
#include "net_app.sha256.inc"
};

int main(void)
{
	printk("\n*** Wi-Fi scenarios: link up, TCP round trip, AP loss ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("netwatch", net_app, ARRAY_SIZE(net_app),
					net_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("verify/load failed (%d)\n", rc);
		return 0;
	}
	rc = app_platform_launch_llext("netwatch", ext, (app_entry_t)entry,
				       APP_CAPS);
	if (rc < 0) {
		printk("launch refused (%d)\n", rc);
		return 0;
	}
	printk("netwatch running as app %d\n", rc);
	return 0;
}
