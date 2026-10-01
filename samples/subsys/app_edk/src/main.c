/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/llext/llext.h>
#include <zephyr/sys/printk.h>
#include <string.h>

static const uint8_t mod_bytes[] = {
#include "app_edk/hello.llext.inc"
};
static const uint8_t bundle[] = {
#include "app_edk/hello.manifest.inc"
};

static uint8_t work[8192] __aligned(8);

int main(void)
{
	size_t mf_len = sizeof(bundle) - APP_MANIFEST_SIG_LEN;
	struct app_platform_status st;
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	int ret;
	int id;

	if (mf_len != sizeof(mf) || sizeof(mod_bytes) > sizeof(work)) {
		printk("app_edk: the bundle does not fit\n");
		return 0;
	}
	memcpy(&mf, bundle, mf_len);
	memcpy(work, mod_bytes, sizeof(mod_bytes));

	if (app_platform_init() != 0) {
		printk("app_edk: platform init failed\n");
		return 0;
	}

	ret = app_loader_verify_load_manifest(&mf, mf_len, bundle + mf_len, APP_MANIFEST_SIG_LEN,
					      work, sizeof(mod_bytes), "app_main", &ext, &entry);
	if (ret != 0) {
		printk("app_edk: module refused: %d\n", ret);
		return 0;
	}
	printk("app_edk: module '%s' v%u signed by key %u verified\n", mf.name, mf.version,
	       mf.key_id);

	id = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, NULL,
				       APP_CAP_LOG | APP_CAP_TIME);
	if (id < 0) {
		printk("app_edk: launch failed: %d\n", id);
		return 0;
	}

	do {
		k_msleep(100);
	} while (app_platform_status(id, &st) == 0 && st.active);
	printk("app_edk: module ended\n");
	return 0;
}
