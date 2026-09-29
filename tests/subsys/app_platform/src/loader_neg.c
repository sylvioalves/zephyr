/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Additional loader negatives: wrong key, truncated, padded, name mismatch,
 * foreign ABI, foreign ELF class or CPU.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
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
static const uint8_t badabi_manifest[] = {
#include "test_mod.badabi.manifest.inc"
};
static const uint8_t badabi_sig[] = {
#include "test_mod.badabi.sig.inc"
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

ZTEST(loader_neg, test_foreign_abi_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;

	memcpy(&mf, badabi_manifest, sizeof(mf));
	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	int rc = app_loader_verify_load_manifest(&mf, badabi_sig, ARRAY_SIZE(badabi_sig), work,
						 ARRAY_SIZE(mod_bytes), mod_pubkey,
						 ARRAY_SIZE(mod_pubkey), "app_main", &ext, &entry);

	zassert_equal(rc, -ENOEXEC, "foreign ABI must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_file_foreign_abi_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	char path[64];

	memcpy(&mf, badabi_manifest, sizeof(mf));
	zassert_equal(app_registry_store_blob("abiapp", mod_bytes, ARRAY_SIZE(mod_bytes)), 0);
	zassert_equal(app_registry_blob_path("abiapp", path, sizeof(path)), 0);
	int rc = app_loader_verify_load_manifest_file(&mf, badabi_sig, ARRAY_SIZE(badabi_sig), path,
						      mod_pubkey, ARRAY_SIZE(mod_pubkey),
						      "app_main", &ext, &entry);

	(void)fs_unlink(path);
	zassert_equal(rc, -ENOEXEC, "foreign ABI must be rejected (%d)", rc);
	zassert_is_null(ext);
}

/* A module for another ELF class or CPU is refused before it is hashed. */
ZTEST(loader_neg, test_foreign_class_and_machine_rejected)
{
	const size_t class_off = 4;
	const size_t machine_off = 18;
	const uint8_t expect[32] = {0};
	struct llext *ext = NULL;
	void *entry = NULL;

	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	work[class_off] ^= 0x3U;
	zassert_equal(app_loader_verify_load("foreign", work, ARRAY_SIZE(mod_bytes), expect,
					     "app_main", &ext, &entry), -E2BIG,
		      "a module of another ELF class must be rejected");
	zassert_is_null(ext);

	memcpy(work, mod_bytes, ARRAY_SIZE(mod_bytes));
	work[machine_off] ^= 0xffU;
	zassert_equal(app_loader_verify_load("foreign", work, ARRAY_SIZE(mod_bytes), expect,
					     "app_main", &ext, &entry), -E2BIG,
		      "a module for another CPU must be rejected");
	zassert_is_null(ext);
}

ZTEST_SUITE(loader_neg, NULL, NULL, NULL, NULL, NULL);
