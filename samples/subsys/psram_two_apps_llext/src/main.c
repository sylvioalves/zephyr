/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two loadable applications, each an independent instance of the same verified
 * llext module, launched as isolated user-mode apps. Each maps its own private
 * region of external RAM (PSRAM) and runs a ten-second concurrent
 * read/write/verify stress over it. The regions are private partitions of each
 * app's own memory domain and are allocated non-overlapping, so the two apps
 * hammer external RAM at the same time without interfering; any corruption
 * surfaces as a verify mismatch.
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

#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_PSRAM)
#define STRESS_MS 10000

static int load_and_launch(const char *name)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load(name, psram_app, ARRAY_SIZE(psram_app),
					psram_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("install '%s' failed: %d\n", name, rc);
		return rc;
	}

	return app_platform_launch_llext(name, ext, (app_entry_t)entry, NULL, APP_CAPS);
}

int main(void)
{
	printk("\n*** two-app loadable external RAM stress (C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	if (load_and_launch("psram-a") < 0 || load_and_launch("psram-b") < 0) {
		return 0;
	}

	/* Let both apps run the full 10 s stress plus reporting time. */
	k_msleep(STRESS_MS + 3000);

	printk("\n*** done: two loadable apps each stressed a private region ***\n");
	return 0;
}
