/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The capabilities a loaded module holds: what its signed manifest requests,
 * bounded by the launcher, or what the launcher allows without a manifest.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/llext/llext.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

static const uint8_t mod_bytes[] = {
#include "test_mod.inc"
};
static const uint8_t caps_manifest[] = {
#include "test_mod.caps.manifest.inc"
};
static const uint8_t caps_sig[] = {
#include "test_mod.caps.sig.inc"
};
static const uint8_t mod_pubkey[] = {
#include "test_mod.pubkey.inc"
};

#define REQUESTED (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_RANDOM)

static uint8_t caps_work[8192] __aligned(8);

static struct llext *load_caps_module(struct app_manifest *mf, void **entry)
{
	struct llext *ext = NULL;

	zassert_true(sizeof(mod_bytes) <= sizeof(caps_work), "module too big");
	memcpy(caps_work, mod_bytes, sizeof(mod_bytes));
	memcpy(mf, caps_manifest, sizeof(*mf));
	zassert_equal(app_loader_verify_load_manifest(mf, sizeof(*mf), caps_sig, sizeof(caps_sig),
						      caps_work, sizeof(mod_bytes), mod_pubkey,
						      sizeof(mod_pubkey), "app_main", &ext, entry),
		      0);
	return ext;
}

static void unload(struct llext *ext)
{
	app_loader_forget(ext);
	llext_unload(&ext);
}

/* Launch stopped, read the capabilities the application holds, remove it. */
static int held_caps(const char *name, struct llext *ext, void *entry, app_caps_t ceiling,
		     app_caps_t *caps)
{
	const struct app_launch_opts opts = {.stopped = true};
	struct app_platform_status st;
	int id = app_platform_launch_llext_opts(name, ext, (app_entry_t)entry, NULL, ceiling,
						&opts);

	if (id < 0) {
		return id;
	}
	zassert_equal(app_platform_status(id, &st), 0);
	*caps = st.caps;
	zassert_equal(app_platform_remove(id), 0);
	for (int i = 0; i < 400 && app_platform_status(id, &st) == 0; i++) {
		k_msleep(5);
	}
	return 0;
}

ZTEST(module_caps, test_manifest_request_bounds_the_ceiling)
{
	struct app_manifest mf;
	app_caps_t caps = 0U;
	void *entry = NULL;
	struct llext *ext = load_caps_module(&mf, &entry);

	zassert_equal(mf.requested_caps, REQUESTED);
	zassert_equal(held_caps(mf.name, ext, entry, APP_CAP_ALL, &caps), 0);
	zassert_equal(caps, REQUESTED, "held %#llx", (unsigned long long)caps);
}

ZTEST(module_caps, test_optional_caps_outside_ceiling_dropped)
{
	struct app_manifest mf;
	app_caps_t caps = 0U;
	void *entry = NULL;
	struct llext *ext = load_caps_module(&mf, &entry);

	zassert_equal(held_caps(mf.name, ext, entry, APP_CAP_LOG | APP_CAP_TIME | APP_CAP_GPIO,
				&caps),
		      0);
	zassert_equal(caps, APP_CAP_LOG | APP_CAP_TIME, "held %#llx", (unsigned long long)caps);
}

ZTEST(module_caps, test_required_caps_outside_ceiling_refused)
{
	struct app_manifest mf;
	app_caps_t caps = 0U;
	void *entry = NULL;
	struct llext *ext = load_caps_module(&mf, &entry);

	zassert_equal(held_caps(mf.name, ext, entry, APP_CAP_LOG | APP_CAP_RANDOM, &caps), -EPERM);
	for (int i = 0; i < app_platform_count(); i++) {
		struct app_platform_status st;

		zassert_false(app_platform_status(i, &st) == 0 && st.name != NULL &&
				      strcmp(st.name, mf.name) == 0,
			      "a refused launch kept a slot");
	}
	unload(ext);
}

ZTEST(module_caps, test_module_without_manifest_holds_ceiling)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	app_caps_t requested;
	app_caps_t required;
	app_caps_t caps = 0U;
	void *entry = NULL;

	memcpy(&mf, caps_manifest, sizeof(mf));
	memcpy(caps_work, mod_bytes, sizeof(mod_bytes));
	zassert_equal(app_loader_verify_load("hashed", caps_work, sizeof(mod_bytes),
					     mf.module_sha256, "app_main", &ext, &entry),
		      0);
	zassert_equal(app_loader_manifest_caps(ext, &requested, &required), -ENOENT);
	zassert_equal(held_caps("hashed", ext, entry, APP_CAP_LOG | APP_CAP_GPIO, &caps), 0);
	zassert_equal(caps, APP_CAP_LOG | APP_CAP_GPIO, "held %#llx", (unsigned long long)caps);
}

static int autostart_with_small_ceiling(const struct app_record *rec, void *user)
{
	struct app_manifest mf;
	void *entry = NULL;
	struct llext *ext;
	int rc;

	ARG_UNUSED(user);
	if (strcmp(rec->name, "testmod") != 0) {
		return -ENOENT;
	}
	ext = load_caps_module(&mf, &entry);
	rc = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, NULL, APP_CAP_LOG);
	if (rc < 0) {
		unload(ext);
	}
	return rc;
}

ZTEST(module_caps, test_refused_autostart_is_recorded)
{
	int err = 1;

	zassert_equal(app_registry_load(), 0);
	zassert_equal(app_registry_install("testmod", true), 0);
	zassert_equal(app_registry_autostart_result("testmod", &err), 0);
	zassert_equal(err, 0);
	zassert_true(app_registry_autostart_all(autostart_with_small_ceiling, NULL) >= 0);
	zassert_equal(app_registry_autostart_result("testmod", &err), 0);
	zassert_equal(err, -EPERM, "recorded %d", err);

	zassert_equal(app_registry_install("testmod", true), 0);
	zassert_equal(app_registry_autostart_result("testmod", &err), 0);
	zassert_equal(err, 0, "a reinstall kept the refusal");
	zassert_equal(app_registry_uninstall("testmod"), 0);
	zassert_equal(app_registry_autostart_result("testmod", &err), -ENOENT);
}

ZTEST_SUITE(module_caps, NULL, NULL, NULL, NULL, NULL);
