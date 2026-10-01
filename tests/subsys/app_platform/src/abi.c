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

#include "module_run.h"

/* Argument that makes the test module check the kernel services and return. */
#define TIME_PROBE ((void *)0x5449U)

/* Argument that makes the test module check 64-bit division and return. */
#define DIV_PROBE ((void *)0x4449U)

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

ZTEST(module_abi, test_kernel_services)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	int id;

	zassert_true(sizeof(mod_bytes) <= sizeof(module_work), "module too big");
	memcpy(module_work, mod_bytes, sizeof(mod_bytes));
	memcpy(&mf, mod_manifest, sizeof(mf));
	zassert_equal(app_loader_verify_load_manifest(&mf, sizeof(mf), mod_sig, sizeof(mod_sig),
						      module_work, sizeof(mod_bytes), mod_pubkey,
						      sizeof(mod_pubkey), "app_main", &ext, &entry),
		      0);

	module_watch(mf.name);
	id = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, TIME_PROBE,
				       APP_CAP_LOG);
	zassert_true(id >= 0, "launch rc %d", id);
	zassert_equal(module_wait_end(id, 3000), 0, "the module did not return");
}

ZTEST(module_abi, test_64bit_division)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	int id;

	memcpy(module_work, mod_bytes, sizeof(mod_bytes));
	memcpy(&mf, mod_manifest, sizeof(mf));
	zassert_equal(app_loader_verify_load_manifest(&mf, sizeof(mf), mod_sig, sizeof(mod_sig),
						      module_work, sizeof(mod_bytes), mod_pubkey,
						      sizeof(mod_pubkey), "app_main", &ext, &entry),
		      0);

	module_watch(mf.name);
	id = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, DIV_PROBE, APP_CAP_LOG);
	zassert_true(id >= 0, "launch rc %d", id);
	zassert_equal(module_wait_end(id, 3000), 0, "the module did not return");
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
