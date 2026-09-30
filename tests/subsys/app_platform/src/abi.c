/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The functions the firmware exports to loadable modules. */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

/* Argument that makes the test module check the kernel services and return. */
#define TIME_PROBE ((void *)0x5449U)

static const uint8_t mod_bytes[] = {
#include "test_mod.inc"
};
static const uint8_t mod_manifest[] = {
#include "test_mod.manifest.inc"
};
static const uint8_t mod_sig[] = {
#include "test_mod.sig.inc"
};
static const uint8_t mod_pubkey[] = {
#include "test_mod.pubkey.inc"
};
static const uint8_t absent_bytes[] __aligned(8) = {
#include "absent_mod.inc"
};

static uint8_t abi_work[8192] __aligned(8);

static int slot_of(const char *name)
{
	for (int i = 0; i < app_platform_count(); i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0 && st.name != NULL &&
		    strcmp(st.name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool wait_slot_free(const char *name, int ms)
{
	for (int i = 0; i < ms / 5; i++) {
		if (slot_of(name) < 0) {
			return true;
		}
		k_msleep(5);
	}
	return slot_of(name) < 0;
}

ZTEST(module_abi, test_kernel_services)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	int id;

	zassert_true(sizeof(mod_bytes) <= sizeof(abi_work), "module too big");
	memcpy(abi_work, mod_bytes, sizeof(mod_bytes));
	memcpy(&mf, mod_manifest, sizeof(mf));
	zassert_equal(app_loader_verify_load_manifest(&mf, sizeof(mf), mod_sig, sizeof(mod_sig),
						      abi_work, sizeof(mod_bytes), mod_pubkey,
						      sizeof(mod_pubkey), "app_main", &ext, &entry),
		      0);

	id = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, TIME_PROBE,
				       APP_CAP_LOG);
	zassert_true(id >= 0, "launch rc %d", id);
	if (!wait_slot_free(mf.name, 3000)) {
		(void)app_platform_remove(id);
		zassert_true(wait_slot_free(mf.name, 2000), "the module was not removed");
		ztest_test_fail();
	}
}

ZTEST(module_abi, test_missing_service_fails_to_link)
{
	struct llext_buf_loader buf_loader =
		LLEXT_TEMPORARY_BUF_LOADER(absent_bytes, sizeof(absent_bytes));
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;

	zassert_equal(llext_load(&buf_loader.loader, "absent", &ext, &ldr_parm), -ENODATA);
	zassert_is_null(ext);
}

ZTEST_SUITE(module_abi, NULL, NULL, NULL, NULL, NULL);
