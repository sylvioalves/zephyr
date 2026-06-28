/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Verify a module and load it as an llext. Two levels are offered: a SHA-256
 * integrity check (detects corruption) and an ECDSA P-256 signature check
 * (proves the module came from the holder of a trusted private key, the path a
 * network-downloaded module must follow). The check runs before the module is
 * loaded, so a tampered, forged or malformed module is rejected and never
 * linked. The module is also structurally bounds-checked before any parsing.
 */

#include <zephyr/app_platform/app_loader.h>

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <psa/crypto.h>
#include <string.h>

LOG_MODULE_DECLARE(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

/* Minimal ELF identification: a relocatable object the llext core can accept. */
#define ELF_MAGIC      0x464c457fU /* 0x7f 'E' 'L' 'F' little-endian */
#define ELF_ET_REL     1
#define ELF_HDR_MIN    52          /* 32-bit ELF header size */
#define ELF_E_TYPE_OFF 16
#define ELF_E_SHOFF_OFF 32
#define ELF_E_SHNUM_OFF 48

static int sha256(const uint8_t *buf, size_t len, uint8_t out[32])
{
	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, buf, len, out, 32, &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
}

/*
 * Reject a buffer that cannot be a valid 32-bit relocatable ELF before the
 * loader touches it: enough bytes for a header, the ELF magic, an ET_REL type,
 * and a section table that lies inside the buffer. This bounds the data the
 * llext parser can be handed even if a signature or hash were somehow met.
 */
static int elf_bounds_ok(const uint8_t *buf, size_t len)
{
	if (len < ELF_HDR_MIN) {
		return -E2BIG;
	}
	if (sys_get_le32(buf) != ELF_MAGIC) {
		return -E2BIG;
	}
	if (sys_get_le16(buf + ELF_E_TYPE_OFF) != ELF_ET_REL) {
		return -E2BIG;
	}

	uint32_t shoff = sys_get_le32(buf + ELF_E_SHOFF_OFF);
	uint16_t shnum = sys_get_le16(buf + ELF_E_SHNUM_OFF);
	/* 32-bit section header entry is 40 bytes. */
	uint64_t sh_end = (uint64_t)shoff + (uint64_t)shnum * 40U;

	if (shoff >= len || sh_end > len) {
		return -E2BIG;
	}
	return 0;
}

static int load_checked(const char *name, const uint8_t *buf, size_t len,
			const char *entry_sym, struct llext **ext, void **entry)
{
	struct llext_buf_loader bl = LLEXT_BUF_LOADER(buf, len);
	struct llext_load_param parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *loaded = NULL;
	int rc = llext_load(&bl.loader, name, &loaded, &parm);

	if (rc != 0) {
		LOG_ERR("llext_load failed for '%s': %d", name, rc);
		return rc;
	}

	void *fn = llext_find_sym(&loaded->exp_tab, entry_sym);

	if (fn == NULL) {
		LOG_ERR("no '%s' entry in '%s'", entry_sym, name);
		llext_unload(&loaded);
		return -ENOENT;
	}

	*ext = loaded;
	*entry = fn;
	return 0;
}

int app_loader_verify_load(const char *name, const uint8_t *buf, size_t len,
			   const uint8_t expect[32], const char *entry_sym,
			   struct llext **ext, void **entry)
{
	uint8_t got[32];

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}
	if (elf_bounds_ok(buf, len) != 0) {
		LOG_WRN("'%s' rejected: not a valid relocatable module", name);
		return -E2BIG;
	}
	if (sha256(buf, len, got) != 0) {
		return -EIO;
	}
	if (memcmp(got, expect, 32) != 0) {
		LOG_WRN("verify failed for '%s' - hash mismatch, rejected", name);
		return -EPERM;
	}
	LOG_INF("verify OK for '%s' (sha-256 matches)", name);

	return load_checked(name, buf, len, entry_sym, ext, entry);
}

int app_loader_verify_load_signed(const char *name, const uint8_t *buf,
				  size_t len, const uint8_t *sig, size_t sig_len,
				  const uint8_t *pubkey, size_t pubkey_len,
				  const char *entry_sym, struct llext **ext,
				  void **entry)
{
	uint8_t digest[32];

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}
	if (elf_bounds_ok(buf, len) != 0) {
		LOG_WRN("'%s' rejected: not a valid relocatable module", name);
		return -E2BIG;
	}
	if (sha256(buf, len, digest) != 0) {
		return -EIO;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attr,
			 PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_status_t st = psa_import_key(&attr, pubkey, pubkey_len, &key);

	if (st != PSA_SUCCESS) {
		LOG_ERR("'%s' pubkey import failed: %d", name, (int)st);
		return -EIO;
	}

	st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest,
			     sizeof(digest), sig, sig_len);
	psa_destroy_key(key);

	if (st != PSA_SUCCESS) {
		LOG_WRN("verify failed for '%s' - bad signature, rejected", name);
		return -EPERM;
	}
	LOG_INF("verify OK for '%s' (ecdsa-p256 signature authentic)", name);

	return load_checked(name, buf, len, entry_sym, ext, entry);
}
