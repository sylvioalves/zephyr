/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Persistent loadable application: an installed app survives a reboot.
 *
 * On the first boot the embedded module bytes stand in for a downloaded
 * release: they are written to the storage partition and recorded in the
 * registry. On every boot - first or later - the app is then loaded from
 * flash (not from the embedded copy), verified against its hash, and
 * launched. After a reset the binary is still on flash and in the registry,
 * so it comes straight back without being downloaded again.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

/* The "downloaded" release bytes, embedded only to seed the first install. */
static const uint8_t persist_app[] = {
#include "persist.inc"
};
static const uint8_t persist_hash[32] = {
#include "persist.sha256.inc"
};

#define APP_NAME "persist"
#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME)

static uint8_t load_buf[8192];

static bool is_installed(const char *name)
{
	for (int i = 0; i < app_registry_count(); i++) {
		struct app_record r;

		if (app_registry_get(i, &r) == 0 && strcmp(r.name, name) == 0) {
			return true;
		}
	}
	return false;
}

int main(void)
{
	printk("\n*** persistent loadable app (C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	if (app_registry_load() != 0) {
		printk("registry load failed\n");
		return 0;
	}

	if (!is_installed(APP_NAME)) {
		printk("first boot: installing '%s' to flash\n", APP_NAME);
		if (app_registry_store_blob(APP_NAME, persist_app,
					    ARRAY_SIZE(persist_app)) != 0 ||
		    app_registry_install(APP_NAME, true) != 0) {
			printk("install failed\n");
			return 0;
		}
		printk("installed and persisted to flash\n");
	} else {
		printk("found '%s' already installed - restored after reboot\n",
		       APP_NAME);
	}

	size_t len = 0;

	if (app_registry_load_blob(APP_NAME, load_buf, sizeof(load_buf),
				   &len) != 0) {
		printk("could not read '%s' binary from flash\n", APP_NAME);
		return 0;
	}
	printk("loaded '%s' from flash (%zu bytes)\n", APP_NAME, len);

	struct llext *ext = NULL;
	void *entry = NULL;

	if (app_loader_verify_load(APP_NAME, load_buf, len, persist_hash,
				   "app_main", &ext, &entry) != 0) {
		printk("verify/load of flash binary failed\n");
		return 0;
	}

	app_platform_launch_llext(APP_NAME, ext, (app_entry_t)entry, APP_CAPS);
	printk("running '%s' from persisted binary\n", APP_NAME);

	for (;;) {
		k_msleep(5000);
		printk("*** '%s' still running; reboot and it returns from "
		       "flash ***\n", APP_NAME);
	}
	return 0;
}
