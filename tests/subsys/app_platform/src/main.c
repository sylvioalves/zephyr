/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Tests for the app_platform registry and verifying loader: the chip-agnostic
 * security logic that gates what gets installed and launched. Runs on hardware
 * with USERSPACE so it exercises the same code paths the platform ships.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
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
static uint8_t work[8192];

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

/*
 * A separately-signed module whose first relocation's r_offset is past its
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
ZTEST_SUITE(loader, NULL, NULL, NULL, NULL, NULL);
