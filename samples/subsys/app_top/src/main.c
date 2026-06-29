/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Launch a fleet of loadable applications, then leave a shell up so their live
 * status can be inspected. Each app is an isolated user-mode instance of the
 * same verified module looping with a one-second period. Run "appstat status"
 * on the console to see the installed and running apps with their stack, module
 * footprint, CPU share, and the system heap.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t app_bytes[] = {
#include "loop_app.inc"
};
static const uint8_t app_hash[32] = {
#include "loop_app.sha256.inc"
};

#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME)
#define N_APPS   10

int main(void)
{
	printk("\n*** app fleet + live status (run: appstat status) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	/* Names must outlive the launch: the platform keeps the pointer. */
	static char names[N_APPS][8];
	int launched = 0;

	for (int i = 0; i < N_APPS; i++) {
		char *name = names[i];

		name[0] = 'a'; name[1] = 'p'; name[2] = 'p';
		name[3] = '0' + (i / 10);
		name[4] = '0' + (i % 10);
		name[5] = '\0';

		struct llext *ext = NULL;
		void *entry = NULL;
		int rc = app_loader_verify_load(name, app_bytes,
						ARRAY_SIZE(app_bytes), app_hash,
						"app_main", &ext, &entry);

		if (rc != 0) {
			printk("load #%d failed: %d\n", i, rc);
			break;
		}
		rc = app_platform_launch_llext(name, ext, (app_entry_t)entry,
					       APP_CAPS);
		if (rc < 0) {
			printk("launch #%d failed: %d\n", i, rc);
			break;
		}
		launched++;
	}

	printk("*** launched %d apps; shell ready, try: appstat status ***\n",
	       launched);
	return 0;
}
