/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App persistence. Installed apps are recorded in a registry on flash, so they
 * survive a reboot: the platform loads the registry at boot and knows what is
 * installed without re-installing. On the first ever boot the registry is empty
 * and two apps are installed; on every boot after that they are simply present.
 *
 * Power-cycle or reset the board to see the installed apps reload from flash and
 * the boot counter advance.
 */

#include "app_registry.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("\n*** app persistence: installed apps survive reboot (C6) ***\n");

	if (app_registry_load() != 0) {
		printk("registry load failed\n");
		return 0;
	}

	int n = app_registry_count();

	if (n == 0) {
		printk("\nfirst boot: installing two apps...\n");
		app_registry_install("thermostat", true);
		app_registry_install("logger", false);
		n = app_registry_count();
	} else {
		printk("\nthis is a REBOOT: %d app(s) already installed, "
		       "no re-install needed\n", n);
	}

	printk("\ninstalled apps (from flash):\n");
	for (int i = 0; i < n; i++) {
		struct app_record r;

		if (app_registry_get(i, &r) == 0) {
			printk("  - %-12s autostart=%d install_seq=%u\n",
			       r.name, r.autostart, r.install_seq);
		}
	}

	printk("\n*** reset the board: the same apps reload from flash ***\n");
	return 0;
}
