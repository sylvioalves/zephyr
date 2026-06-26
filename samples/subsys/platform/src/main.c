/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The integrated platform. This is the assembled system, not a single feature:
 * at boot it loads the installed-app registry from flash, launches each
 * installed app as an isolated user-mode group, and supervises them. The apps
 * use platform services; a misbehaving app is detected and reclaimed while the
 * others and the system keep running. The whole boot -> install -> run ->
 * survive-a-fault story in one image.
 */

#include "platform.h"
#include "app_registry.h"
#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern int manager_init(void);
extern int manager_launch(const struct app_def *def);
extern int loader_install(const char *name, uint32_t caps);

int main(void)
{
	printk("\n================ Zephyr Application Platform ================\n");

	if (manager_init() != 0) {
		printk("platform: manager init failed\n");
		return 0;
	}

	/* Bring up the brokered devices before any app can call a service. */
	svc_devices_init();

	/* Load what is installed (persisted across reboots). */
	if (app_registry_load() != 0) {
		printk("platform: registry load failed\n");
		return 0;
	}

	if (app_registry_count() == 0) {
		printk("\nfirst boot: installing apps...\n");
		app_registry_install("climate", true);
		app_registry_install("datalogger", true);
		app_registry_install("blinker", true);
		app_registry_install("buggy", true);
	} else {
		printk("\nreboot: %d app(s) already installed\n",
		       app_registry_count());
	}

	/* Launch every installed, autostart app from the catalog. */
	printk("\nlaunching installed apps:\n");
	int n = app_registry_count();

	for (int i = 0; i < n; i++) {
		struct app_record r;

		if (app_registry_get(i, &r) != 0 || !r.autostart) {
			continue;
		}
		const struct app_def *def = platform_catalog_find(r.name);

		if (def == NULL) {
			printk("  (no catalog entry for '%s')\n", r.name);
			continue;
		}
		manager_launch(def);
	}

	/* Install a runtime-downloaded llext app: verify, load, isolate, run. */
	printk("\ninstalling a runtime-loaded app (llext):\n");
	loader_install("installed-llext", CAP_GPIO);

	/* Let the system run: good apps tick, the buggy one is reclaimed. */
	printk("\n--- platform running ---\n");
	k_msleep(6000);

	printk("\n=== platform still alive: a bad app was reclaimed, the rest ran ===\n");
	return 0;
}
