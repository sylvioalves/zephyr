/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Tests for the app_platform registry and verifying loader: the chip-agnostic
 * security logic that gates what gets installed and launched. Runs on hardware
 * with USERSPACE so it exercises the same code paths the platform ships.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <errno.h>
#include <string.h>

/* Embedded signed test module: bytes, manifest, signature, trusted key. */
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

/* A mutable copy used to craft malformed variants. */
static uint8_t work[8192] __aligned(8);

static struct app_manifest manifest_copy(void)
{
	struct app_manifest mf;

	memcpy(&mf, mod_manifest, sizeof(mf));
	return mf;
}

static size_t fresh_module(void)
{
	zassert_true(ARRAY_SIZE(mod_bytes) <= sizeof(work), "module too big");
	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	return ARRAY_SIZE(mod_bytes);
}

static int load_with(const struct app_manifest *mf, const uint8_t *sig,
		     size_t sig_len, uint8_t *buf, size_t len,
		     struct llext **ext)
{
	void *entry = NULL;

	return app_loader_verify_load_manifest(mf, sig, sig_len, buf, len,
					       mod_pubkey, ARRAY_SIZE(mod_pubkey),
					       "app_main", ext, &entry);
}

/* ---- registry ---- */

ZTEST(registry, test_install_find_uninstall)
{
	zassert_equal(app_registry_install("alpha", true), 0);
	zassert_equal(app_registry_install("beta", false), 0);

	bool found_alpha = false;

	for (int i = 0; i < app_registry_count(); i++) {
		struct app_record r;

		zassert_equal(app_registry_get(i, &r), 0);
		if (strcmp(r.name, "alpha") == 0) {
			found_alpha = true;
		}
	}
	zassert_true(found_alpha, "installed app not found");

	zassert_equal(app_registry_uninstall("alpha"), 0);
	zassert_equal(app_registry_uninstall("alpha"), -ENOENT,
		      "second uninstall should report not found");
}

ZTEST(registry, test_version_rollback)
{
	zassert_equal(app_registry_install_versioned("verapp", true, 5), 0);
	/* A lower version must be refused. */
	zassert_equal(app_registry_install_versioned("verapp", true, 4), -EACCES,
		      "downgrade should be rejected");
	/* Equal or higher is accepted. */
	zassert_equal(app_registry_install_versioned("verapp", true, 5), 0);
	zassert_equal(app_registry_install_versioned("verapp", true, 6), 0);

	zassert_equal(app_registry_uninstall("verapp"), 0);
}

ZTEST(registry, test_blob_roundtrip)
{
	const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t out[16];
	size_t out_len = 0;

	zassert_equal(app_registry_store_blob("blobapp", data, sizeof(data)), 0);
	zassert_equal(app_registry_load_blob("blobapp", out, sizeof(out),
					     &out_len), 0);
	zassert_equal(out_len, sizeof(data), "blob length mismatch");
	zassert_mem_equal(out, data, sizeof(data), "blob content mismatch");

	(void)app_registry_install("blobapp", false);
	zassert_equal(app_registry_uninstall("blobapp"), 0);
	/* Uninstall removes the blob. */
	zassert_equal(app_registry_load_blob("blobapp", out, sizeof(out),
					     &out_len), -ENOENT,
		      "blob should be gone after uninstall");
}

ZTEST(registry, test_manifest_roundtrip)
{
	struct app_manifest mf = manifest_copy();
	struct app_manifest got;
	uint8_t sig[APP_MANIFEST_SIG_LEN];

	zassert_equal(app_registry_store_manifest("mfapp", &mf, mod_sig,
						  ARRAY_SIZE(mod_sig)), 0);
	zassert_equal(app_registry_load_manifest("mfapp", &got, sig,
						 sizeof(sig)), 0);
	zassert_mem_equal(&got, &mf, sizeof(mf), "manifest mismatch");
	zassert_mem_equal(sig, mod_sig, APP_MANIFEST_SIG_LEN, "sig mismatch");

	(void)app_registry_install("mfapp", false);
	zassert_equal(app_registry_uninstall("mfapp"), 0);
}

/* Stage a manifest and a one-byte-pattern module, as a transport would. */
static struct app_manifest stage(const char *name, uint32_t version, uint8_t fill)
{
	struct app_manifest mf = manifest_copy();
	uint8_t blob[32];
	char path[64];
	struct fs_file_t f;

	memset(blob, fill, sizeof(blob));
	memset(mf.name, 0, sizeof(mf.name));
	strncpy(mf.name, name, sizeof(mf.name) - 1);
	mf.version = version;
	mf.module_len = sizeof(blob);
	memset(mf.module_sha256, fill, sizeof(mf.module_sha256));

	zassert_equal(app_registry_store_staged_manifest(name, &mf, mod_sig,
							 ARRAY_SIZE(mod_sig)), 0);
	zassert_equal(app_registry_staged_blob_path(name, path, sizeof(path)), 0);
	fs_file_t_init(&f);
	zassert_equal(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC), 0);
	zassert_equal(fs_write(&f, blob, sizeof(blob)), sizeof(blob));
	fs_close(&f);
	return mf;
}

static bool get_record(const char *name, struct app_record *out)
{
	for (int i = 0; i < app_registry_count(); i++) {
		if (app_registry_get(i, out) == 0 && strcmp(out->name, name) == 0) {
			return true;
		}
	}
	return false;
}

static bool staged_present(const char *name)
{
	char path[64];
	struct fs_dirent st;

	zassert_equal(app_registry_staged_blob_path(name, path, sizeof(path)), 0);
	if (fs_stat(path, &st) == 0) {
		return true;
	}
	zassert_equal(app_registry_staged_manifest_path(name, path, sizeof(path)), 0);
	return fs_stat(path, &st) == 0;
}

ZTEST(registry, test_commit_installs_staged_image)
{
	struct app_manifest mf = stage("stapp", 3, 0xA3);
	struct app_manifest got;
	struct app_record rec;
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	uint8_t out[64];
	size_t out_len = 0;

	zassert_equal(app_registry_commit("stapp", false, &mf), 0);
	zassert_false(staged_present("stapp"), "staged files left behind");
	zassert_equal(app_registry_load_blob("stapp", out, sizeof(out), &out_len), 0);
	zassert_equal(out_len, 32U);
	zassert_equal(out[0], 0xA3, "installed module is not the staged one");
	zassert_equal(app_registry_load_manifest("stapp", &got, sig, sizeof(sig)), 0);
	zassert_equal(got.version, 3U);
	zassert_true(get_record("stapp", &rec));
	zassert_equal(rec.version, 3U);
	zassert_equal(rec.bound, 1U, "record not bound to the module hash");

	zassert_equal(app_registry_uninstall("stapp"), 0);
}

ZTEST(registry, test_commit_downgrade_keeps_installed_image)
{
	struct app_manifest mf = stage("dnapp", 5, 0x55);
	struct app_manifest got;
	struct app_record rec;
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	uint8_t out[64];
	size_t out_len = 0;

	zassert_equal(app_registry_commit("dnapp", false, &mf), 0);

	mf = stage("dnapp", 4, 0x44);
	zassert_equal(app_registry_commit("dnapp", false, &mf), -EACCES,
		      "downgrade should be refused");
	zassert_false(staged_present("dnapp"), "refused image not discarded");
	zassert_equal(app_registry_load_blob("dnapp", out, sizeof(out), &out_len), 0);
	zassert_equal(out[0], 0x55, "refused downgrade replaced the installed module");
	zassert_equal(app_registry_load_manifest("dnapp", &got, sig, sizeof(sig)), 0);
	zassert_equal(got.version, 5U, "refused downgrade replaced the manifest");
	zassert_true(get_record("dnapp", &rec));
	zassert_equal(rec.version, 5U);

	mf = stage("otherapp", 9, 0x99);
	zassert_equal(app_registry_commit("dnapp", false, &mf), -EINVAL,
		      "a manifest for another app was committed");
	app_registry_discard("otherapp");

	zassert_equal(app_registry_uninstall("dnapp"), 0);
}

ZTEST(registry, test_check_refuses_other_images)
{
	struct app_manifest mf = stage("ckapp", 7, 0x77);
	struct app_manifest probe;
	struct app_record rec;

	zassert_equal(app_registry_commit("ckapp", false, &mf), 0);
	zassert_true(get_record("ckapp", &rec));

	zassert_equal(app_registry_check(&rec, &mf), 0, "committed image refused");

	probe = mf;
	probe.version = 6;
	zassert_equal(app_registry_check(&rec, &probe), -EACCES,
		      "older signed image accepted at boot");

	probe = mf;
	probe.module_sha256[0] ^= 0xFF;
	zassert_equal(app_registry_check(&rec, &probe), -EBADMSG,
		      "image other than the committed one accepted");

	probe = mf;
	strcpy(probe.name, "someone");
	zassert_equal(app_registry_check(&rec, &probe), -EINVAL);

	zassert_equal(app_registry_uninstall("ckapp"), 0);
}

ZTEST(registry, test_commit_survives_reload)
{
	struct app_manifest mf = stage("rlapp", 2, 0x22);
	struct app_record rec;

	zassert_equal(app_registry_commit("rlapp", true, &mf), 0);
	zassert_equal(app_registry_load(), 0);
	zassert_true(get_record("rlapp", &rec), "record lost across reload");
	zassert_equal(rec.version, 2U);
	zassert_equal(rec.bound, 1U);
	zassert_mem_equal(rec.module_sha256, mf.module_sha256, sizeof(rec.module_sha256));

	zassert_equal(app_registry_uninstall("rlapp"), 0);
}

/* ---- loader ---- */

ZTEST(loader, test_genuine_loads)
{
	struct app_manifest mf = manifest_copy();
	size_t len = fresh_module();
	struct llext *ext = NULL;

	zassert_equal(load_with(&mf, mod_sig, ARRAY_SIZE(mod_sig), work, len,
				&ext), 0, "genuine module should load");
	zassert_not_null(ext);
	zassert_true(app_loader_is_verified(ext), "should be marked verified");
	app_loader_forget(ext);
	llext_unload(&ext);
}

ZTEST(loader, test_same_name_loaded_twice)
{
	struct app_manifest mf = manifest_copy();
	size_t len = fresh_module();
	struct llext *first = NULL;
	struct llext *second = NULL;

	zassert_equal(load_with(&mf, mod_sig, ARRAY_SIZE(mod_sig), work, len,
				&first), 0);
	len = fresh_module();
	zassert_equal(load_with(&mf, mod_sig, ARRAY_SIZE(mod_sig), work, len,
				&second), -EALREADY,
		      "a second load of a loaded name must be refused");
	zassert_is_null(second, "the loaded extension was handed back");

	app_loader_forget(first);
	llext_unload(&first);
	zassert_is_null(llext_by_name(mf.name),
			"a reference leaked: the extension outlived its unload");
}

static int load_file_with(const uint8_t *bytes, size_t len, struct llext **ext)
{
	struct app_manifest mf = manifest_copy();
	char path[64];
	void *entry = NULL;
	int rc;

	zassert_equal(app_registry_store_blob("fileapp", bytes, len), 0);
	zassert_equal(app_registry_blob_path("fileapp", path, sizeof(path)), 0);
	rc = app_loader_verify_load_manifest_file(&mf, mod_sig, ARRAY_SIZE(mod_sig), path,
						  mod_pubkey, ARRAY_SIZE(mod_pubkey),
						  "app_main", ext, &entry);
	(void)fs_unlink(path);
	return rc;
}

ZTEST(loader, test_file_genuine_loads)
{
	size_t len = fresh_module();
	struct llext *ext = NULL;

	zassert_equal(load_file_with(work, len, &ext), 0, "genuine file should load");
	zassert_true(app_loader_is_verified(ext));
	app_loader_forget(ext);
	llext_unload(&ext);
}

ZTEST(loader, test_file_longer_than_signed_rejected)
{
	size_t len = fresh_module();
	struct llext *ext = NULL;

	work[len] = 0;
	zassert_equal(load_file_with(work, len + 1U, &ext), -EPERM);
	zassert_is_null(ext);
}

ZTEST(loader, test_file_tampered_rejected)
{
	size_t len = fresh_module();
	struct llext *ext = NULL;

	work[len / 2] ^= 0xff;
	zassert_equal(load_file_with(work, len, &ext), -EPERM);
	zassert_is_null(ext);
}

ZTEST(loader, test_tampered_module_rejected)
{
	struct app_manifest mf = manifest_copy();
	size_t len = fresh_module();
	struct llext *ext = NULL;

	/* Flip a byte: module no longer matches the signed manifest hash. */
	work[len / 2] ^= 0xff;
	zassert_equal(load_with(&mf, mod_sig, ARRAY_SIZE(mod_sig), work, len,
				&ext), -EPERM, "tampered module must be rejected");
	zassert_is_null(ext);
}

ZTEST(loader, test_bad_signature_rejected)
{
	struct app_manifest mf = manifest_copy();
	size_t len = fresh_module();
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	struct llext *ext = NULL;

	memcpy(sig, mod_sig, sizeof(sig));
	sig[0] ^= 0xff; /* corrupt the signature */
	zassert_equal(load_with(&mf, sig, sizeof(sig), work, len, &ext), -EPERM,
		      "bad signature must be rejected");
	zassert_is_null(ext);
}

ZTEST(loader, test_bad_manifest_magic_rejected)
{
	struct app_manifest mf = manifest_copy();
	size_t len = fresh_module();
	struct llext *ext = NULL;

	mf.magic ^= 0xffffffff; /* corrupting the manifest breaks its signature */
	zassert_not_equal(load_with(&mf, mod_sig, ARRAY_SIZE(mod_sig), work, len,
				    &ext), 0, "altered manifest must be rejected");
	zassert_is_null(ext);
}

ZTEST(loader, test_length_mismatch_rejected)
{
	struct app_manifest mf = manifest_copy();
	size_t len = fresh_module();
	struct llext *ext = NULL;

	/* Present fewer bytes than the manifest binds. */
	zassert_not_equal(load_with(&mf, mod_sig, ARRAY_SIZE(mod_sig), work,
				    len - 16, &ext), 0,
			  "length mismatch must be rejected");
	zassert_is_null(ext);
}

/* A separately-signed module whose first relocation's r_offset is past its
 * target section. Its manifest and signature are valid - the bytes were signed
 * after corruption - so it passes hash and signature verification and reaches
 * the loader, where the relocation bounds check must reject it. This proves the
 * bounds check, not the hash check.
 */
static const uint8_t evil_bytes[] = {
#include "evil_mod.inc"
};
static const uint8_t evil_manifest[] = {
#include "evil_mod.manifest.inc"
};
static const uint8_t evil_sig[] = {
#include "evil_mod.sig.inc"
};

ZTEST(loader, test_oob_relocation_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, evil_manifest, sizeof(mf));
	zassert_true(ARRAY_SIZE(evil_bytes) <= sizeof(work), "evil too big");
	memcpy(work, evil_bytes, ARRAY_SIZE(evil_bytes));

	/* Manifest+signature are valid for these bytes, so verification passes
	 * and the loader links it - where the relocation bounds check rejects the
	 * out-of-section r_offset with -ENOEXEC.
	 */
	int rc = load_with(&mf, evil_sig, ARRAY_SIZE(evil_sig), work,
			   ARRAY_SIZE(evil_bytes), &ext);

	zassert_equal(rc, -ENOEXEC,
		      "out-of-section relocation must be rejected (got %d)", rc);
	zassert_is_null(ext);
}

static void *registry_setup(void)
{
	/* Mount storage and load the registry once for the suite. */
	zassert_equal(app_registry_load(), 0, "registry load failed");
	return NULL;
}

ZTEST_SUITE(registry, NULL, registry_setup, NULL, NULL, NULL);
ZTEST_SUITE(loader, NULL, registry_setup, NULL, NULL, NULL);

/* ---- lifecycle / reaper ---- */

/* Built-in test apps. The platform reclaims a faulted or finished app and a
 * dedicated reaper-helper thread releases its resources off the supervisor.
 * These tests prove the supervisor reclaims, the reaper completes cleanup, and
 * the slot becomes reusable - the contract of the detached-cleanup design.
 */
/* These run as user-mode apps, so they must not touch kernel memory (no shared
 * counters): the test observes the lifecycle through app_platform_status, which
 * is exactly the contract being verified.
 */
static void crasher_app(void *arg)
{
	ARG_UNUSED(arg);
	/* Fault on purpose: a NULL write traps and routes through the fatal
	 * handler, which flags the app and signals the supervisor.
	 */
	volatile uint32_t *p = NULL;
	*p = 0xdead;
}

static void quitter_app(void *arg)
{
	ARG_UNUSED(arg);
	/* Return immediately: a clean exit, reclaimed but never relaunched. */
}

/* Find the slot id currently holding the named app, or -1. */
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

/* Wait until the named app's slot is free (reaper finished), bounded. */
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

ZTEST(lifecycle, test_restart_refused_for_loaded_app)
{
	struct app_manifest mf = manifest_copy();
	size_t len = fresh_module();
	struct llext *ext = NULL;
	void *entry = NULL;

	zassert_equal(app_loader_verify_load_manifest(&mf, mod_sig, ARRAY_SIZE(mod_sig), work,
						      len, mod_pubkey, ARRAY_SIZE(mod_pubkey),
						      "app_main", &ext, &entry), 0);

	int id = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, NULL, APP_CAP_LOG);

	zassert_true(id >= 0, "launch failed (%d)", id);
	zassert_equal(app_platform_set_restart(id, true), -ENOTSUP,
		      "a loaded application was marked for restart");
	zassert_equal(app_platform_set_restart(id, false), 0);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free(mf.name, 2000), "removed app slot not freed");
}

ZTEST(lifecycle, test_fault_is_reclaimed)
{
	struct app_def def = {
		.name = "crasher",
		.source = APP_BUILTIN,
		.entry = crasher_app,
		.caps = APP_CAP_LOG,
	};

	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed (%d)", id);

	/* The app crashes; the supervisor reclaims it and the reaper frees the
	 * slot. The slot must become free without help from the test.
	 */
	zassert_true(wait_slot_free("crasher", 2000),
		     "faulted app slot was never reclaimed/cleaned");
}

ZTEST(lifecycle, test_clean_exit_is_reclaimed)
{
	struct app_def def = {
		.name = "quitter",
		.source = APP_BUILTIN,
		.entry = quitter_app,
		.caps = APP_CAP_LOG,
	};

	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed (%d)", id);
	zassert_true(wait_slot_free("quitter", 2000),
		     "cleanly-exited app slot was never reclaimed");
}

ZTEST(lifecycle, test_slot_reusable_after_reclaim)
{
	struct app_def def = {
		.name = "recycle",
		.source = APP_BUILTIN,
		.entry = quitter_app,
		.caps = APP_CAP_LOG,
	};

	/* Launch, let it finish and be reaped, then relaunch into a reusable
	 * slot. Repeat to prove the reaper actually frees slots and the platform
	 * does not leak them.
	 */
	for (int round = 0; round < 8; round++) {
		int id = app_platform_launch(&def);

		zassert_true(id >= 0, "round %d launch failed (%d)", round, id);
		zassert_true(wait_slot_free("recycle", 2000),
			     "round %d slot not reclaimed", round);
	}
}

#define ARG_MAGIC ((void *)0x5a5aU)

static void arg_checker_app(void *arg)
{
	while (arg == ARG_MAGIC) {
		k_msleep(10);
	}
}

ZTEST(lifecycle, test_entry_receives_argument)
{
	struct app_def def = {
		.name = "argapp",
		.source = APP_BUILTIN,
		.entry = arg_checker_app,
		.arg = ARG_MAGIC,
		.caps = APP_CAP_LOG,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed (%d)", id);
	k_msleep(100);
	zassert_equal(slot_of("argapp"), id, "the entry did not receive its argument");
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("argapp", 2000));

	def.arg = NULL;
	id = app_platform_launch(&def);
	zassert_true(id >= 0, "launch failed (%d)", id);
	zassert_true(wait_slot_free("argapp", 2000), "the entry ignored its argument");
}

static void polite_app(void *arg)
{
	ARG_UNUSED(arg);
	while (app_svc_stop_requested() == 0) {
		k_msleep(5);
	}
}

static void stubborn_app(void *arg)
{
	ARG_UNUSED(arg);
	for (;;) {
		k_msleep(5);
	}
}

ZTEST(lifecycle, test_request_stop)
{
	struct app_def def = {
		.name = "polite",
		.source = APP_BUILTIN,
		.entry = polite_app,
		.caps = APP_CAP_LOG,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed (%d)", id);
	k_msleep(20);
	zassert_equal(app_platform_request_stop(id, 1000), 0, "a cooperating app was forced");
	zassert_true(wait_slot_free("polite", 2000));

	def.name = "stubborn";
	def.entry = stubborn_app;
	id = app_platform_launch(&def);
	zassert_true(id >= 0, "launch failed (%d)", id);
	k_msleep(20);
	zassert_equal(app_platform_request_stop(id, 100), -ETIMEDOUT);
	zassert_true(wait_slot_free("stubborn", 2000));
}

ZTEST(lifecycle, test_launch_stopped)
{
	struct app_def def = {
		.name = "held",
		.source = APP_BUILTIN,
		.entry = quitter_app,
		.caps = APP_CAP_LOG,
		.stopped = true,
	};
	struct app_platform_status st;
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed (%d)", id);
	k_msleep(50);
	zassert_equal(app_platform_status(id, &st), 0, "a stopped launch ran and exited");
	zassert_false(st.active, "a stopped launch is running");
	zassert_equal(app_platform_launch(&def), -EEXIST);
	if (IS_ENABLED(CONFIG_USERSPACE)) {
		zassert_not_null(app_platform_domain(id));
	}
	zassert_equal(app_platform_start(id), id);
	zassert_true(wait_slot_free("held", 2000), "the started application did not run");

	id = app_platform_launch(&def);
	zassert_true(id >= 0, "launch failed (%d)", id);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("held", 2000), "a never started application kept its slot");
}

ZTEST(lifecycle, test_launch_stopped_module)
{
	const struct app_launch_opts opts = { .stopped = true };
	struct app_platform_status st;

	for (int round = 0; round < 2; round++) {
		struct app_manifest mf = manifest_copy();
		size_t len = fresh_module();
		struct llext *ext = NULL;
		void *entry = NULL;
		int id;

		zassert_equal(app_loader_verify_load_manifest(&mf, mod_sig, ARRAY_SIZE(mod_sig),
							      work, len, mod_pubkey,
							      ARRAY_SIZE(mod_pubkey), "app_main",
							      &ext, &entry), 0);
		id = app_platform_launch_llext_opts(mf.name, ext, (app_entry_t)entry, NULL,
						    APP_CAP_LOG, &opts);
		zassert_true(id >= 0, "launch failed (%d)", id);
		zassert_equal(app_platform_status(id, &st), 0);
		zassert_false(st.active, "a stopped launch is running");
		zassert_true(st.module_bytes > 0U, "the module is not mapped");
		if (round == 1) {
			zassert_equal(app_platform_start(id), id);
		}
		zassert_equal(app_platform_remove(id), 0);
		zassert_true(wait_slot_free(mf.name, 2000), "round %d slot not freed", round);
	}
}

#ifdef CONFIG_USERSPACE
K_APPMEM_PARTITION_DEFINE(relaunch_part);
K_APP_BMEM(relaunch_part) static volatile uint32_t relaunch_runs;
#else
static volatile uint32_t relaunch_runs;
#endif

/* Fault on the first run only, then keep running. */
static void fault_once_app(void *arg)
{
	ARG_UNUSED(arg);
	if (relaunch_runs++ == 0U) {
		crasher_app(NULL);
	}
	for (;;) {
		k_msleep(10);
	}
}

ZTEST(lifecycle, test_fault_relaunch_keeps_slot)
{
	struct app_def filler = {
		.name = "filler",
		.source = APP_BUILTIN,
		.entry = quitter_app,
		.caps = APP_CAP_LOG,
		.stopped = true,
	};
	struct app_def def = {
		.name = "recrash",
		.source = APP_BUILTIN,
		.entry = fault_once_app,
		.caps = APP_CAP_LOG,
		.stopped = true,
	};
	struct k_mem_domain *dom;
	struct app_platform_status st;
	bool relaunched = false;
	int fid = app_platform_launch(&filler);
	int id = app_platform_launch(&def);

	zassert_true(fid >= 0 && id > fid, "launch failed (%d, %d)", fid, id);
	relaunch_runs = 0U;
	dom = app_platform_domain(id);
#ifdef CONFIG_USERSPACE
	zassert_equal(k_mem_domain_add_partition(dom, &relaunch_part), 0);
#endif
	zassert_equal(app_platform_set_restart(id, true), 0);
	zassert_equal(app_platform_remove(fid), 0);
	zassert_true(wait_slot_free("filler", 2000));
	zassert_equal(app_platform_start(id), id);

	for (int i = 0; i < 400 && !relaunched; i++) {
		relaunched = app_platform_status(id, &st) == 0 && st.active && st.restarts == 1U &&
			     strcmp(st.name, "recrash") == 0;
		k_msleep(5);
	}
	zassert_true(relaunched, "the fault relaunch did not take the same slot");
	k_msleep(50);
	zassert_equal(relaunch_runs, 2U, "the relaunch ran %u times", relaunch_runs);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("recrash", 2000));
#ifdef CONFIG_USERSPACE
	zassert_equal(k_mem_domain_remove_partition(dom, &relaunch_part), 0);
#else
	ARG_UNUSED(dom);
#endif
}

#ifdef CONFIG_USERSPACE
K_APPMEM_PARTITION_DEFINE(grant_part);
K_APP_BMEM(grant_part) static volatile uint32_t grant_runs;
K_APP_BMEM(grant_part) static volatile int grant_rc[2];
K_APP_BMEM(grant_part) static volatile int ctl_rc[2];
K_APP_BMEM(grant_part) static volatile bool ctl_done;
#else
static volatile uint32_t grant_runs;
static volatile int grant_rc[2];
static volatile int ctl_rc[2];
static volatile bool ctl_done;
#endif

static atomic_t configured;

static int grant_window(int id, void *user)
{
	ARG_UNUSED(user);
	atomic_inc(&configured);
	app_platform_grant_flash(id, 0U, 4096U);
	return 0;
}

static int map_results(int id, void *user)
{
	ARG_UNUSED(user);
	atomic_inc(&configured);
#ifdef CONFIG_USERSPACE
	return k_mem_domain_add_partition(app_platform_domain(id), &grant_part);
#else
	ARG_UNUSED(id);
	return 0;
#endif
}

static int refuse_start(int id, void *user)
{
	ARG_UNUSED(id);
	ARG_UNUSED(user);
	atomic_inc(&configured);
	return -EIO;
}

/* Record the flash read of each run, and fault on the first run when @p arg
 * is not NULL.
 */
static void flash_reader_app(void *arg)
{
	uint8_t buf[4];
	uint32_t run = grant_runs;

	if (run < ARRAY_SIZE(grant_rc)) {
		grant_rc[run] = app_svc_flash_read(0U, buf, sizeof(buf));
	}
	grant_runs = run + 1U;
	if (arg != NULL && run == 0U) {
		crasher_app(NULL);
	}
	for (;;) {
		k_msleep(10);
	}
}

static void control_app(void *arg)
{
	int target = (int)(uintptr_t)arg;

	ctl_rc[0] = app_svc_app_start(target);
	ctl_rc[1] = app_svc_app_restart(target);
	ctl_done = true;
	for (;;) {
		k_msleep(10);
	}
}

static bool wait_runs(uint32_t runs, int ms)
{
	for (int i = 0; i < ms / 5 && grant_runs < runs; i++) {
		k_msleep(5);
	}
	return grant_runs >= runs;
}

static void reset_results(void)
{
	grant_runs = 0U;
	ctl_done = false;
	for (size_t i = 0; i < ARRAY_SIZE(grant_rc); i++) {
		grant_rc[i] = 1;
		ctl_rc[i] = 1;
	}
	atomic_set(&configured, 0);
}

ZTEST(lifecycle, test_configure_grants_survive_fault_relaunch)
{
	struct app_def def = {
		.name = "grantfault",
		.source = APP_BUILTIN,
		.entry = flash_reader_app,
		.arg = (void *)1,
		.caps = APP_CAP_LOG | APP_CAP_FLASH,
		.stopped = true,
		.configure = grant_window,
	};
	struct k_mem_domain *dom;
	struct app_platform_status st;
	bool relaunched = false;
	int id;

	reset_results();
	id = app_platform_launch(&def);
	zassert_true(id >= 0, "launch failed (%d)", id);
	zassert_equal(atomic_get(&configured), 0, "a stopped launch ran the hook");
	dom = app_platform_domain(id);
#ifdef CONFIG_USERSPACE
	zassert_equal(k_mem_domain_add_partition(dom, &grant_part), 0);
#endif
	zassert_equal(app_platform_set_restart(id, true), 0);
	zassert_equal(app_platform_start(id), id);

	for (int i = 0; i < 400 && !relaunched; i++) {
		relaunched = grant_runs >= 2U && app_platform_status(id, &st) == 0 &&
			     st.restarts == 1U;
		k_msleep(5);
	}
	zassert_true(relaunched, "the application was not relaunched");
	zassert_equal(grant_rc[0], 0, "first run read %d", grant_rc[0]);
	zassert_equal(grant_rc[1], 0, "relaunch read %d", grant_rc[1]);
	zassert_equal(atomic_get(&configured), 2);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("grantfault", 2000));
#ifdef CONFIG_USERSPACE
	zassert_equal(k_mem_domain_remove_partition(dom, &grant_part), 0);
#else
	ARG_UNUSED(dom);
#endif
}

ZTEST(lifecycle, test_configure_grants_survive_stop_start)
{
	struct app_def def = {
		.name = "grantstop",
		.source = APP_BUILTIN,
		.entry = flash_reader_app,
		.caps = APP_CAP_LOG | APP_CAP_FLASH,
		.stopped = true,
		.configure = grant_window,
	};
	struct k_mem_domain *dom;
	int id;

	reset_results();
	id = app_platform_launch(&def);
	zassert_true(id >= 0, "launch failed (%d)", id);
	dom = app_platform_domain(id);
#ifdef CONFIG_USERSPACE
	zassert_equal(k_mem_domain_add_partition(dom, &grant_part), 0);
#endif
	zassert_equal(app_platform_start(id), id);
	zassert_true(wait_runs(1U, 2000), "the application did not run");
	zassert_equal(app_platform_stop(id), 0);
	zassert_equal(app_platform_start(id), id);
	zassert_true(wait_runs(2U, 2000), "the application did not run again");
	zassert_equal(grant_rc[0], 0, "first run read %d", grant_rc[0]);
	zassert_equal(grant_rc[1], 0, "second run read %d", grant_rc[1]);
	zassert_equal(atomic_get(&configured), 2);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("grantstop", 2000));
#ifdef CONFIG_USERSPACE
	zassert_equal(k_mem_domain_remove_partition(dom, &grant_part), 0);
#else
	ARG_UNUSED(dom);
#endif
}

ZTEST(lifecycle, test_control_start_of_held_app)
{
	struct app_def target = {
		.name = "heldtgt",
		.source = APP_BUILTIN,
		.entry = quitter_app,
		.caps = APP_CAP_LOG,
		.stopped = true,
	};
	struct app_def ctl = {
		.name = "ctl",
		.source = APP_BUILTIN,
		.entry = control_app,
		.caps = APP_CAP_LOG | APP_CAP_CONTROL,
		.configure = map_results,
	};
	struct app_platform_status st;
	struct k_mem_domain *dom;
	int tid;
	int cid;

	reset_results();
	tid = app_platform_launch(&target);
	zassert_true(tid >= 0, "launch failed (%d)", tid);
	ctl.arg = (void *)(uintptr_t)tid;
	cid = app_platform_launch(&ctl);
	zassert_true(cid >= 0, "launch failed (%d)", cid);
	dom = app_platform_domain(cid);
	zassert_equal(atomic_get(&configured), 1, "the launch did not run the hook");

	for (int i = 0; i < 400 && !ctl_done; i++) {
		k_msleep(5);
	}
	zassert_true(ctl_done, "the control application did not run");
	zassert_equal(ctl_rc[0], -EAGAIN, "control start returned %d", ctl_rc[0]);
	zassert_equal(ctl_rc[1], -EAGAIN, "control restart returned %d", ctl_rc[1]);
	zassert_equal(app_platform_status(tid, &st), 0);
	zassert_false(st.active, "a held application was started");

	zassert_equal(app_platform_start(tid), tid);
	zassert_true(wait_slot_free("heldtgt", 2000), "the firmware start did not run it");
	zassert_equal(app_platform_remove(cid), 0);
	zassert_true(wait_slot_free("ctl", 2000));
#ifdef CONFIG_USERSPACE
	zassert_equal(k_mem_domain_remove_partition(dom, &grant_part), 0);
#else
	ARG_UNUSED(dom);
#endif
}

ZTEST(lifecycle, test_configure_failure)
{
	struct app_def def = {
		.name = "refused",
		.source = APP_BUILTIN,
		.entry = quitter_app,
		.caps = APP_CAP_LOG,
		.configure = refuse_start,
	};
	struct app_platform_status st;
	int id;

	reset_results();
	zassert_equal(app_platform_launch(&def), -EIO);
	zassert_true(wait_slot_free("refused", 2000), "a refused launch kept its slot");

	def.stopped = true;
	id = app_platform_launch(&def);
	zassert_true(id >= 0, "launch failed (%d)", id);
	zassert_equal(app_platform_start(id), -EIO);
	zassert_equal(app_platform_status(id, &st), 0);
	zassert_false(st.active, "a refused start is running");
	zassert_equal(atomic_get(&configured), 2);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("refused", 2000));
}

static void file_writer_app(void *arg)
{
	uint8_t buf[256];

	ARG_UNUSED(arg);
	for (size_t i = 0; i < sizeof(buf); i++) {
		buf[i] = (uint8_t)i;
	}
	for (;;) {
		int h = app_svc_file_open("churn.bin", 1);

		if (h < 0) {
			continue;
		}
		for (int i = 0; i < 8; i++) {
			(void)app_svc_file_write(h, buf, sizeof(buf));
		}
		(void)app_svc_file_close(h);
	}
}

ZTEST(lifecycle, test_remove_during_service_call)
{
	struct app_def def = {
		.name = "fswriter",
		.source = APP_BUILTIN,
		.entry = file_writer_app,
		.caps = APP_CAP_FS,
	};

	/* The writer spends most of its time inside the file service, so the
	 * removals land in the middle of service calls.
	 */
	for (int round = 0; round < 20; round++) {
		int id = app_platform_launch(&def);

		zassert_true(id >= 0, "round %d launch failed (%d)", round, id);
		k_msleep(20 + round * 13);
		zassert_equal(app_platform_remove(id), 0);
		zassert_true(wait_slot_free("fswriter", 3000),
			     "round %d slot not reclaimed", round);
	}

	struct fs_dirent st;

	zassert_equal(fs_stat("/lfs/apps/fswriter/churn.bin", &st), 0,
		      "the writer never reached the file service");
	zassert_true(st.size > 0U);
}

static void *lifecycle_setup(void)
{
	zassert_equal(app_registry_load(), 0, "registry load failed");
	zassert_equal(app_platform_init(), 0, "platform init failed");
	return NULL;
}

ZTEST_SUITE(lifecycle, NULL, lifecycle_setup, NULL, NULL, NULL);
