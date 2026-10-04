/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Three isolated applications cooperating across the network and the in-device
 * message bus:
 *   app 1 (pub)    - publishes the SoC die temperature to the broker every 5s
 *   app 2 (bridge) - subscribes to that topic, receives each value over MQTT,
 *                    and forwards it to app 3 over inter-application IPC
 *   app 3 (printer)- receives the forwarded values over IPC and prints them
 * Each runs in its own memory domain and reaches the system only through
 * capability-gated service syscalls.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t pub_app[] __aligned(8) = {
#include "pub_app.inc"
};
static const uint8_t pub_hash[32] = {
#include "pub_app.sha256.inc"
};
static const uint8_t bridge_app[] __aligned(8) = {
#include "bridge_app.inc"
};
static const uint8_t bridge_hash[32] = {
#include "bridge_app.sha256.inc"
};
static const uint8_t print_app[] __aligned(8) = {
#include "print_app.inc"
};
static const uint8_t print_hash[32] = {
#include "print_app.sha256.inc"
};

static int launch(const char *name, const uint8_t *bytes, size_t len,
		   const uint8_t *hash, app_caps_t caps)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load(name, bytes, len, hash, "app_main",
					&ext, &entry);

	if (rc != 0) {
		printk("%s: load failed %d\n", name, rc);
		return rc;
	}
	rc = app_platform_launch_llext(name, ext, (app_entry_t)entry, NULL, caps);
	if (rc < 0) {
		printk("%s: %s failed %d\n", name, __func__, rc);
	}
	return rc;
}

int main(void)
{
	printk("\n*** three cooperating apps: MQTT -> IPC chain ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	/* TRIO_APPS is a bitmask selecting which apps to launch, so each can be
	 * brought up alone or together: bit0 pub, bit1 bridge, bit2 printer.
	 * Default: all three.
	 */
#ifndef TRIO_APPS
#define TRIO_APPS 0x7
#endif
	printk("trio: launching set 0x%x\n", TRIO_APPS);

	/* Printer first so its IPC name is registered before the bridge sends. */
	if (TRIO_APPS & 0x4) {
		launch("printer", print_app, ARRAY_SIZE(print_app), print_hash,
		       APP_CAP_LOG | APP_CAP_IPC);
		k_msleep(200);
	}
	if (TRIO_APPS & 0x2) {
		launch("bridge", bridge_app, ARRAY_SIZE(bridge_app), bridge_hash,
		       APP_CAP_LOG | APP_CAP_NET | APP_CAP_IPC);
	}
	if (TRIO_APPS & 0x1) {
		launch("pub", pub_app, ARRAY_SIZE(pub_app), pub_hash,
		       APP_CAP_LOG | APP_CAP_NET | APP_CAP_TIME | APP_CAP_SENSOR);
	}

	printk("*** launched set 0x%x; watch the flow ***\n", TRIO_APPS);
	return 0;
}
