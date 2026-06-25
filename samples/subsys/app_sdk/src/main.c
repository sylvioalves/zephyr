/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App SDK demo. The platform exports one clean API surface (app_sdk.h). A
 * loadable module - compiled separately, knowing only that header - is loaded
 * at runtime and calls the SDK; it can reach only what the platform exported.
 * The capability boundary is shown two ways: the loadable app is denied Wi-Fi
 * configuration (not admin), while a trusted system app is allowed it.
 */

#include "app_sdk.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>

extern void app_sdk_set_admin(bool admin);

static uint8_t module_buf[] = {
#include "demo_app.inc"
};

static void load_and_run_app(void)
{
	struct llext_buf_loader bl = LLEXT_BUF_LOADER(module_buf,
						      sizeof(module_buf));
	struct llext_load_param parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;

	printk("\n=== loading a downloaded app (knows only app_sdk.h) ===\n");

	if (llext_load(&bl.loader, "demo", &ext, &parm) != 0) {
		printk("load failed\n");
		return;
	}

	void (*entry)(void) = llext_find_sym(&ext->exp_tab, "app_main");

	if (entry == NULL) {
		printk("no app_main\n");
		llext_unload(&ext);
		return;
	}

	/* A downloaded app is NOT admin. */
	app_sdk_set_admin(false);
	entry();

	llext_unload(&ext);
	printk("=== app unloaded ===\n");
}

static void trusted_system_app(void)
{
	printk("\n=== a trusted system app (granted admin) ===\n");
	app_sdk_set_admin(true);

	int rc = app_admin_wifi_set_credentials("HomeNet", "secret123");

	printk("system app wifi-config rc=%d (%s)\n", rc,
	       rc == 0 ? "ALLOWED" : "denied");
	app_sdk_set_admin(false);
}

int main(void)
{
	printk("\n*** app SDK: clean API surface, loadable apps, capability boundary ***\n");

	load_and_run_app();
	trusted_system_app();

	printk("\n*** done: app coded only against app_sdk.h; admin gated by capability ***\n");
	return 0;
}
