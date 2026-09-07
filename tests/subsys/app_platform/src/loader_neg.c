/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Additional loader negatives: wrong key, truncated, padded, name mismatch. */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/kernel.h>
#include <string.h>

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

static uint8_t work[8192 + 256];

static int load_key(const struct app_manifest *mf, const uint8_t *key,
		    size_t key_len, uint8_t *buf, size_t len, struct llext **ext)
{
	void *entry = NULL;

	return app_loader_verify_load_manifest(mf, mod_sig, ARRAY_SIZE(mod_sig), buf,
					       len, key, key_len, "app_main", ext,
					       &entry);
}

ZTEST(loader_neg, test_wrong_key_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	uint8_t key[65];

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	memcpy(key, mod_pubkey, sizeof(key));
	key[10] ^= 0x55; /* a different (invalid or foreign) public point */
	zassert_equal(load_key(&mf, key, sizeof(key), work, ARRAY_SIZE(mod_bytes),
			       &ext), -EPERM, "wrong key must be rejected");
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_truncated_module_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	/* Manifest untouched, module cut short: length binds first. */
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), work,
			  ARRAY_SIZE(mod_bytes) / 2, &ext);

	zassert_equal(rc, -EPERM, "truncated module must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_padded_module_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	memset(work + ARRAY_SIZE(mod_bytes), 0x90, 256);
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), work,
			  ARRAY_SIZE(mod_bytes) + 256, &ext);

	zassert_equal(rc, -EPERM, "padded module must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_name_mismatch_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	strncpy(mf.name, "impostor", sizeof(mf.name) - 1);
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), work,
			  ARRAY_SIZE(mod_bytes), &ext);

	zassert_equal(rc, -EPERM, "renamed manifest must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_version_field_tamper_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	mf.version += 1; /* bump the version without re-signing */
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), work,
			  ARRAY_SIZE(mod_bytes), &ext);

	zassert_equal(rc, -EPERM, "version tamper must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST_SUITE(loader_neg, NULL, NULL, NULL, NULL, NULL);
