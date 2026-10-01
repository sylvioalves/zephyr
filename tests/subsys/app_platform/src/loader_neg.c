/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Additional loader negatives: wrong key, truncated, padded, name mismatch,
 * foreign ABI or target, foreign ELF class, CPU or ELF flags, forbidden imports
 * and the manifest format.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform_abi.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "module_run.h"

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
static const uint8_t long_manifest[] = {
#include "test_mod.long.manifest.inc"
};
static const uint8_t long_sig[] = {
#include "test_mod.long.sig.inc"
};
static const uint8_t short_manifest[] = {
#include "test_mod.short.manifest.inc"
};
static const uint8_t short_sig[] = {
#include "test_mod.short.sig.inc"
};
static const uint8_t target_manifest[] = {
#include "test_mod.target.manifest.inc"
};
static const uint8_t target_sig[] = {
#include "test_mod.target.sig.inc"
};
static const uint8_t forbidden_bytes[] = {
#include "forbidden_mod.inc"
};
static const uint8_t forbidden_manifest[] = {
#include "forbidden_mod.manifest.inc"
};
static const uint8_t forbidden_sig[] = {
#include "forbidden_mod.sig.inc"
};

#define MANIFEST_V3_MAGIC 0x33464d41U
#define MANIFEST_V3_LEN   124U

static int load_key(const struct app_manifest *mf, const uint8_t *key,
		    size_t key_len, uint8_t *buf, size_t len, struct llext **ext)
{
	void *entry = NULL;

	return app_loader_verify_load_manifest(mf, sizeof(*mf), mod_sig, ARRAY_SIZE(mod_sig), buf,
					       len, key, key_len, "app_main", ext,
					       &entry);
}

ZTEST(loader_neg, test_wrong_key_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	uint8_t key[65];

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	memcpy(key, mod_pubkey, sizeof(key));
	key[10] ^= 0x55; /* a different (invalid or foreign) public point */
	zassert_equal(load_key(&mf, key, sizeof(key), module_work, ARRAY_SIZE(mod_bytes),
			       &ext), -EPERM, "wrong key must be rejected");
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_truncated_module_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	/* Manifest untouched, module cut short: length binds first. */
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), module_work,
			  ARRAY_SIZE(mod_bytes) / 2, &ext);

	zassert_equal(rc, -EPERM, "truncated module must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_padded_module_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	memset(module_work + ARRAY_SIZE(mod_bytes), 0x90, 256);
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), module_work,
			  ARRAY_SIZE(mod_bytes) + 256, &ext);

	zassert_equal(rc, -EPERM, "padded module must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_name_mismatch_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	strncpy(mf.name, "impostor", sizeof(mf.name) - 1);
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), module_work,
			  ARRAY_SIZE(mod_bytes), &ext);

	zassert_equal(rc, -EPERM, "renamed manifest must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_version_field_tamper_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	mf.version += 1; /* bump the version without re-signing */
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), module_work,
			  ARRAY_SIZE(mod_bytes), &ext);

	zassert_equal(rc, -EPERM, "version tamper must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_heap_size_tamper_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;

	memcpy(&mf, mod_manifest, sizeof(mf));
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	mf.heap_size += 4096U; /* enlarge the heap without re-signing */
	int rc = load_key(&mf, mod_pubkey, ARRAY_SIZE(mod_pubkey), module_work,
			  ARRAY_SIZE(mod_bytes), &ext);

	zassert_equal(rc, -EPERM, "heap size tamper must be rejected (%d)", rc);
	zassert_is_null(ext);
}

ZTEST(loader_neg, test_foreign_abi_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;

	memcpy(&mf, badabi_manifest, sizeof(mf));
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	int rc = app_loader_verify_load_manifest(&mf, sizeof(mf), badabi_sig,
						 ARRAY_SIZE(badabi_sig), module_work,
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
	int rc = app_loader_verify_load_manifest_file(&mf, sizeof(mf), badabi_sig,
						      ARRAY_SIZE(badabi_sig), path, mod_pubkey,
						      ARRAY_SIZE(mod_pubkey), "app_main", &ext,
						      &entry);

	(void)fs_unlink(path);
	zassert_equal(rc, -ENOEXEC, "foreign ABI must be rejected (%d)", rc);
	zassert_is_null(ext);
}

/* A module signed for another target is refused. */
ZTEST(loader_neg, test_foreign_target_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;

	BUILD_ASSERT(APP_PLATFORM_TARGET != 0U, "the foreign target must differ");

	memcpy(&mf, target_manifest, sizeof(mf));
	zassert_equal(mf.target, 0U);
	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	int rc = app_loader_verify_load_manifest(&mf, sizeof(mf), target_sig,
						 ARRAY_SIZE(target_sig), module_work,
						 ARRAY_SIZE(mod_bytes), mod_pubkey,
						 ARRAY_SIZE(mod_pubkey), "app_main", &ext, &entry);

	zassert_equal(rc, -ENOEXEC, "foreign target must be rejected (%d)", rc);
	zassert_is_null(ext);

	memcpy(&mf, mod_manifest, sizeof(mf));
	zassert_equal(mf.target, APP_PLATFORM_TARGET, "the signer used another target");
}

/* A module importing a symbol outside the exported interface is refused before
 * it is linked, from memory and from a file.
 */
ZTEST(loader_neg, test_forbidden_import_rejected)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	char path[64];
	int rc;

	memcpy(&mf, forbidden_manifest, sizeof(mf));
	zassert_true(ARRAY_SIZE(forbidden_bytes) <= sizeof(module_work), "module too big");
	memcpy(module_work, forbidden_bytes, ARRAY_SIZE(forbidden_bytes));
	rc = app_loader_verify_load_manifest(&mf, sizeof(mf), forbidden_sig,
					     ARRAY_SIZE(forbidden_sig), module_work,
					     ARRAY_SIZE(forbidden_bytes), mod_pubkey,
					     ARRAY_SIZE(mod_pubkey), "app_main", &ext, &entry);
	zassert_equal(rc, -EACCES, "forbidden import must be rejected (%d)", rc);
	zassert_is_null(ext);

	zassert_equal(app_registry_store_blob("forbidden", forbidden_bytes,
					      ARRAY_SIZE(forbidden_bytes)), 0);
	zassert_equal(app_registry_blob_path("forbidden", path, sizeof(path)), 0);
	rc = app_loader_verify_load_manifest_file(&mf, sizeof(mf), forbidden_sig,
						  ARRAY_SIZE(forbidden_sig), path, mod_pubkey,
						  ARRAY_SIZE(mod_pubkey), "app_main", &ext, &entry);
	(void)fs_unlink(path);
	zassert_equal(rc, -EACCES, "forbidden import must be rejected from a file (%d)", rc);
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

	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	module_work[class_off] ^= 0x3U;
	zassert_equal(app_loader_verify_load("foreign", module_work, ARRAY_SIZE(mod_bytes), expect,
					     "app_main", &ext, &entry), -E2BIG,
		      "a module of another ELF class must be rejected");
	zassert_is_null(ext);

	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	module_work[machine_off] ^= 0xffU;
	zassert_equal(app_loader_verify_load("foreign", module_work, ARRAY_SIZE(mod_bytes), expect,
					     "app_main", &ext, &entry), -E2BIG,
		      "a module for another CPU must be rejected");
	zassert_is_null(ext);
}

/* A module built for another floating-point ABI is refused before it is
 * hashed.
 */
ZTEST(loader_neg, test_foreign_elf_flags_rejected)
{
#if defined(CONFIG_RISCV) || defined(CONFIG_ARM)
	const size_t flags_off = IS_ENABLED(CONFIG_64BIT) ? 48 : 36;
	const uint32_t flip = IS_ENABLED(CONFIG_RISCV)     ? 0x2U
			      : IS_ENABLED(__ARM_PCS_VFP) ? 0x200U
							  : 0x400U;
	const uint8_t expect[32] = {0};
	struct llext *ext = NULL;
	void *entry = NULL;

	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	sys_put_le32(sys_get_le32(&module_work[flags_off]) ^ flip, &module_work[flags_off]);
	zassert_equal(app_loader_verify_load("foreign", module_work, ARRAY_SIZE(mod_bytes), expect,
					     "app_main", &ext, &entry), -ENOEXEC,
		      "a module for another float ABI must be rejected");
	zassert_is_null(ext);
#else
	ztest_test_skip();
#endif
}

ZTEST_SUITE(loader_neg, NULL, NULL, NULL, NULL, NULL);

static int load_mf(const union app_manifest_buf *mb, size_t mf_len, const uint8_t *sig,
		   struct llext **ext)
{
	void *entry = NULL;

	memcpy(module_work, mod_bytes, ARRAY_SIZE(mod_bytes));
	return app_loader_verify_load_manifest(&mb->mf, mf_len, sig, APP_MANIFEST_SIG_LEN,
					       module_work, ARRAY_SIZE(mod_bytes), mod_pubkey,
					       ARRAY_SIZE(mod_pubkey), "app_main", ext, &entry);
}

static int load_mf_file(const char *name, const union app_manifest_buf *mb, size_t mf_len,
			const uint8_t *sig, struct llext **ext)
{
	union app_manifest_buf got;
	uint8_t got_sig[APP_MANIFEST_SIG_LEN];
	size_t got_len = 0;
	void *entry = NULL;
	char path[64];
	int rc;

	zassert_equal(app_registry_store_manifest(name, &mb->mf, mf_len, sig,
						  APP_MANIFEST_SIG_LEN), 0);
	zassert_equal(app_registry_store_blob(name, mod_bytes, ARRAY_SIZE(mod_bytes)), 0);
	zassert_equal(app_registry_load_manifest(name, &got, &got_len, got_sig, sizeof(got_sig)),
		      0);
	zassert_equal(got_len, mf_len, "stored manifest length changed");
	zassert_mem_equal(got.raw, mb->raw, mf_len, "stored manifest changed");
	zassert_equal(app_registry_blob_path(name, path, sizeof(path)), 0);
	rc = app_loader_verify_load_manifest_file(&got.mf, got_len, got_sig, sizeof(got_sig), path,
						  mod_pubkey, ARRAY_SIZE(mod_pubkey), "app_main",
						  ext, &entry);
	(void)app_registry_install(name, false);
	zassert_equal(app_registry_uninstall(name), 0);
	return rc;
}

/* A manifest carrying fields past the ones this firmware knows loads, and the
 * signature covers those fields.
 */
ZTEST(manifest_format, test_longer_manifest_loads)
{
	union app_manifest_buf mb;
	struct llext *ext = NULL;

	BUILD_ASSERT(sizeof(long_manifest) > sizeof(struct app_manifest));
	BUILD_ASSERT(sizeof(long_manifest) <= APP_MANIFEST_MAX_LEN);

	memcpy(mb.raw, long_manifest, sizeof(long_manifest));
	zassert_equal(mb.mf.hdr_len, sizeof(long_manifest));
	zassert_equal(load_mf(&mb, sizeof(long_manifest), long_sig, &ext), 0);
	zassert_true(app_loader_is_verified(ext));
	app_loader_forget(ext);
	llext_unload(&ext);

	zassert_equal(load_mf_file("longapp", &mb, sizeof(long_manifest), long_sig, &ext), 0);
	zassert_true(app_loader_is_verified(ext));
	app_loader_forget(ext);
	llext_unload(&ext);

	mb.raw[sizeof(long_manifest) - 1] ^= 0xffU;
	zassert_equal(load_mf(&mb, sizeof(long_manifest), long_sig, &ext), -EPERM,
		      "an unknown field is not covered by the signature");
	zassert_is_null(ext);
}

/* A manifest of the previous format, as an older firmware installed it, asks
 * for the application to be installed again.
 */
ZTEST(manifest_format, test_format_3_refused)
{
	union app_manifest_buf mb;
	struct llext *ext = NULL;

	memset(&mb, 0, sizeof(mb));
	memcpy(mb.raw, mod_manifest, MANIFEST_V3_LEN);
	sys_put_le32(MANIFEST_V3_MAGIC, mb.raw);

	zassert_equal(load_mf(&mb, MANIFEST_V3_LEN, mod_sig, &ext), -ENOTSUP);
	zassert_is_null(ext);
	zassert_equal(load_mf_file("oldapp", &mb, MANIFEST_V3_LEN, mod_sig, &ext), -ENOTSUP);
	zassert_is_null(ext);
}

/* A manifest shorter than the fields this firmware knows is refused even when
 * it is signed, as is one presented with another length than it states.
 */
ZTEST(manifest_format, test_short_manifest_refused)
{
	union app_manifest_buf mb;
	struct llext *ext = NULL;

	BUILD_ASSERT(sizeof(short_manifest) < sizeof(struct app_manifest));

	memset(&mb, 0, sizeof(mb));
	memcpy(mb.raw, short_manifest, sizeof(short_manifest));
	zassert_equal(mb.mf.hdr_len, sizeof(short_manifest));
	zassert_equal(load_mf(&mb, sizeof(short_manifest), short_sig, &ext), -EBADMSG);
	zassert_is_null(ext);

	memcpy(mb.raw, mod_manifest, sizeof(mod_manifest));
	zassert_equal(load_mf(&mb, sizeof(mod_manifest) - 1U, mod_sig, &ext), -EBADMSG);
	zassert_equal(load_mf(&mb, sizeof(mod_manifest) + 1U, mod_sig, &ext), -EBADMSG);
	zassert_equal(load_mf(&mb, APP_MANIFEST_MAX_LEN + 1U, mod_sig, &ext), -EBADMSG);
	zassert_is_null(ext);
}

ZTEST_SUITE(manifest_format, NULL, NULL, NULL, NULL, NULL);
