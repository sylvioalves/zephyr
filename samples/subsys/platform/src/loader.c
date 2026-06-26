/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Install a downloadable app: the module bytes and expected hash are embedded
 * for the demo. The platform's loader verifies and loads it; the platform then
 * launches it as an isolated user-mode group with the granted capabilities.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>

#include <zephyr/kernel.h>

static const uint8_t module_buf[] = {
#include "installed_app.inc"
};

static const uint8_t expected_hash[32] = {
#include "installed_app.sha256.inc"
};

int loader_install(const char *name, uint32_t caps)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load(name, module_buf, ARRAY_SIZE(module_buf),
					expected_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		return rc;
	}

	return app_platform_launch_llext(name, ext, (app_entry_t)entry, caps);
}
