/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The trusted key table of test_keys.yaml: key 1 holds every capability, key 2
 * holds LOG and signs only names starting with b_, key 3 is revoked, and the
 * development key is accepted only with CONFIG_APP_PLATFORM_DEV_KEY.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/llext/llext.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

#include "module_run.h"

static const uint8_t mod_bytes[] = {
#include "test_mod.inc"
};
static const uint8_t a_manifest[] = {
#include "test_mod.manifest.inc"
};
static const uint8_t a_sig[] = {
#include "test_mod.sig.inc"
};
static const uint8_t a_caps_manifest[] = {
#include "test_mod.caps.manifest.inc"
};
static const uint8_t a_caps_sig[] = {
#include "test_mod.caps.sig.inc"
};
static const uint8_t dev_manifest[] = {
#include "test_mod.dev.manifest.inc"
};
static const uint8_t dev_sig[] = {
#include "test_mod.dev.sig.inc"
};
static const uint8_t unknown_manifest[] = {
#include "test_mod.unknown.manifest.inc"
};
static const uint8_t unknown_sig[] = {
#include "test_mod.unknown.sig.inc"
};
static const uint8_t revoked_manifest[] = {
#include "test_mod.revoked.manifest.inc"
};
static const uint8_t revoked_sig[] = {
#include "test_mod.revoked.sig.inc"
};
static const uint8_t b_manifest[] = {
#include "test_mod.b.manifest.inc"
};
static const uint8_t b_sig[] = {
#include "test_mod.b.sig.inc"
};
static const uint8_t breq_manifest[] = {
#include "test_mod.breq.manifest.inc"
};
static const uint8_t breq_sig[] = {
#include "test_mod.breq.sig.inc"
};
static const uint8_t bname_manifest[] = {
#include "test_mod.bname.manifest.inc"
};
static const uint8_t bname_sig[] = {
#include "test_mod.bname.sig.inc"
};

#define REQUESTED (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_RANDOM)

static int load(const uint8_t *mf_bytes, const uint8_t *sig, struct app_manifest *mf,
		struct llext **ext, void **entry)
{
	zassert_true(sizeof(mod_bytes) <= sizeof(module_work), "module too big");
	memcpy(module_work, mod_bytes, sizeof(mod_bytes));
	memcpy(mf, mf_bytes, sizeof(*mf));
	*ext = NULL;
	return app_loader_verify_load_manifest(mf, sizeof(*mf), sig, APP_MANIFEST_SIG_LEN,
					       module_work, sizeof(mod_bytes), "app_main", ext,
					       entry);
}

static int load_file(const char *name, const uint8_t *mf_bytes, const uint8_t *sig,
		     struct app_manifest *mf, struct llext **ext, void **entry)
{
	char path[64];
	int rc;

	memcpy(mf, mf_bytes, sizeof(*mf));
	zassert_equal(app_registry_store_blob(name, mod_bytes, sizeof(mod_bytes)), 0);
	zassert_equal(app_registry_blob_path(name, path, sizeof(path)), 0);
	*ext = NULL;
	rc = app_loader_verify_load_manifest_file(mf, sizeof(*mf), sig, APP_MANIFEST_SIG_LEN, path,
						  "app_main", ext, entry);
	(void)fs_unlink(path);
	return rc;
}

static void unload(struct llext *ext)
{
	app_loader_forget(ext);
	llext_unload(&ext);
}

/* Launch stopped under a launcher ceiling of every capability. */
static int launch_stopped(const char *name, struct llext *ext, void *entry, app_caps_t *caps)
{
	const struct app_launch_opts opts = {.stopped = true};
	struct app_platform_status st;
	int id = app_platform_launch_llext_opts(name, ext, (app_entry_t)entry, NULL, APP_CAP_ALL,
						&opts);

	if (id >= 0) {
		zassert_equal(app_platform_status(id, &st), 0);
		*caps = st.caps;
	}
	return id;
}

static void remove_app(int id)
{
	struct app_platform_status st;

	zassert_equal(app_platform_remove(id), 0);
	for (int i = 0; i < 400 && app_platform_status(id, &st) == 0; i++) {
		k_msleep(5);
	}
}

ZTEST(module_keys, test_listed_key_is_recorded)
{
	struct app_manifest mf;
	struct llext *ext;
	void *entry = NULL;
	app_caps_t key_caps = 0U;
	uint32_t key_id = 0U;

	zassert_equal(load(a_manifest, a_sig, &mf, &ext, &entry), 0);
	zassert_equal(mf.key_id, 1U);
	zassert_equal(app_loader_key(ext, &key_id, &key_caps), 0);
	zassert_equal(key_id, 1U);
	zassert_equal(key_caps, APP_CAP_ALL);
	unload(ext);
}

ZTEST(module_keys, test_development_key)
{
	struct app_manifest mf;
	struct llext *ext;
	void *entry = NULL;
	app_caps_t key_caps = 0U;
	uint32_t key_id = 1U;
	int rc = load(dev_manifest, dev_sig, &mf, &ext, &entry);

	zassert_equal(mf.key_id, 0U);
	if (!IS_ENABLED(CONFIG_APP_PLATFORM_DEV_KEY)) {
		zassert_equal(rc, -ENXIO, "the development key was accepted (%d)", rc);
		zassert_is_null(ext);
		return;
	}
	zassert_equal(rc, 0);
	zassert_equal(app_loader_key(ext, &key_id, &key_caps), 0);
	zassert_equal(key_id, 0U);
	zassert_equal(key_caps, APP_CAP_ALL);
	unload(ext);
}

ZTEST(module_keys, test_unknown_key_refused)
{
	struct app_manifest mf;
	struct llext *ext;
	void *entry = NULL;

	zassert_equal(load(unknown_manifest, unknown_sig, &mf, &ext, &entry), -ENXIO);
	zassert_equal(mf.key_id, 7U);
	zassert_is_null(ext);
}

ZTEST(module_keys, test_revoked_key_refused)
{
	struct app_manifest mf;
	struct llext *ext;
	void *entry = NULL;

	zassert_equal(load(revoked_manifest, revoked_sig, &mf, &ext, &entry), -ECANCELED);
	zassert_equal(mf.key_id, 3U);
	zassert_is_null(ext);
	zassert_equal(load_file("revoked", revoked_manifest, revoked_sig, &mf, &ext, &entry),
		      -ECANCELED);
	zassert_is_null(ext);
}

/* A manifest naming a listed key it was not signed with. */
ZTEST(module_keys, test_signature_of_another_key_refused)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;

	memcpy(module_work, mod_bytes, sizeof(mod_bytes));
	memcpy(&mf, b_manifest, sizeof(mf));
	mf.key_id = 1U;
	zassert_equal(app_loader_verify_load_manifest(&mf, sizeof(mf), b_sig, sizeof(b_sig),
						      module_work, sizeof(mod_bytes), "app_main",
						      &ext, &entry),
		      -EPERM);
	zassert_is_null(ext);
}

ZTEST(module_keys, test_name_outside_prefix_refused)
{
	struct app_manifest mf;
	struct llext *ext;
	void *entry = NULL;

	zassert_equal(load(bname_manifest, bname_sig, &mf, &ext, &entry), -EPERM);
	zassert_equal(mf.key_id, 2U);
	zassert_is_null(ext);
}

ZTEST(module_keys, test_key_ceiling_bounds_caps)
{
	struct app_manifest mf;
	struct llext *ext;
	void *entry = NULL;
	app_caps_t caps = 0U;
	int id;

	zassert_equal(load(b_manifest, b_sig, &mf, &ext, &entry), 0);
	zassert_equal(mf.requested_caps, REQUESTED);
	id = launch_stopped(mf.name, ext, entry, &caps);
	zassert_true(id >= 0, "launch failed (%d)", id);
	zassert_equal(caps, APP_CAP_LOG, "held %#llx", (unsigned long long)caps);
	remove_app(id);
}

ZTEST(module_keys, test_required_cap_outside_key_ceiling_refused)
{
	struct app_manifest mf;
	struct llext *ext;
	void *entry = NULL;
	app_caps_t caps = 0U;

	zassert_equal(load(breq_manifest, breq_sig, &mf, &ext, &entry), 0);
	zassert_equal(launch_stopped(mf.name, ext, entry, &caps), -EPERM);
	unload(ext);
}

ZTEST(module_keys, test_two_publishers_load)
{
	struct app_manifest mf_a;
	struct app_manifest mf_b;
	struct llext *ext_a;
	struct llext *ext_b;
	void *entry_a = NULL;
	void *entry_b = NULL;
	app_caps_t caps_a = 0U;
	app_caps_t caps_b = 0U;
	int id_a;
	int id_b;

	zassert_equal(load(a_caps_manifest, a_caps_sig, &mf_a, &ext_a, &entry_a), 0);
	zassert_equal(load_file("b_mod", b_manifest, b_sig, &mf_b, &ext_b, &entry_b), 0);
	id_a = launch_stopped(mf_a.name, ext_a, entry_a, &caps_a);
	zassert_true(id_a >= 0, "launch of key 1 failed (%d)", id_a);
	id_b = launch_stopped(mf_b.name, ext_b, entry_b, &caps_b);
	zassert_true(id_b >= 0, "launch of key 2 failed (%d)", id_b);
	zassert_equal(caps_a, REQUESTED, "key 1 held %#llx", (unsigned long long)caps_a);
	zassert_equal(caps_b, APP_CAP_LOG, "key 2 held %#llx", (unsigned long long)caps_b);
	remove_app(id_a);
	remove_app(id_b);
}

static int autostart_from_registry(const struct app_record *rec, void *user)
{
	union app_manifest_buf mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	size_t mf_len = 0;
	struct llext *ext = NULL;
	void *entry = NULL;
	char path[64];
	int rc;

	ARG_UNUSED(user);
	if (strcmp(rec->name, "testmod") != 0) {
		return -ENOENT;
	}
	rc = app_registry_load_manifest(rec->name, &mf, &mf_len, sig, sizeof(sig));
	if (rc == 0) {
		rc = app_registry_blob_path(rec->name, path, sizeof(path));
	}
	if (rc == 0) {
		rc = app_loader_verify_load_manifest_file(&mf.mf, mf_len, sig, sizeof(sig), path,
							  "app_main", &ext, &entry);
	}
	if (rc == 0) {
		rc = app_platform_launch_llext(mf.mf.name, ext, (app_entry_t)entry, NULL,
					       APP_CAP_ALL);
		if (rc < 0) {
			unload(ext);
		}
	}
	return rc;
}

ZTEST(module_keys, test_refused_key_recorded_at_autostart)
{
	const struct {
		const uint8_t *mf;
		const uint8_t *sig;
		int err;
	} cases[] = {
		{revoked_manifest, revoked_sig, -ECANCELED},
		{unknown_manifest, unknown_sig, -ENXIO},
	};
	int err;

	zassert_equal(app_registry_load(), 0);
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_equal(app_registry_store_blob("testmod", mod_bytes, sizeof(mod_bytes)), 0);
		zassert_equal(app_registry_store_manifest("testmod",
							  (const struct app_manifest *)cases[i].mf,
							  sizeof(struct app_manifest), cases[i].sig,
							  APP_MANIFEST_SIG_LEN),
			      0);
		zassert_equal(app_registry_install("testmod", true), 0);
		err = 1;
		zassert_true(app_registry_autostart_all(autostart_from_registry, NULL) >= 0);
		zassert_equal(app_registry_autostart_result("testmod", &err), 0);
		zassert_equal(err, cases[i].err, "recorded %d", err);
		zassert_equal(app_registry_uninstall("testmod"), 0);
	}
}

ZTEST_SUITE(module_keys, NULL, NULL, NULL, NULL, NULL);
