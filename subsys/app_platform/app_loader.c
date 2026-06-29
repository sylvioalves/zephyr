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
#include <zephyr/app_platform/app_manifest.h>

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/llext/fs_loader.h>
#include <zephyr/fs/fs.h>
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

/* Verify an ECDSA P-256 signature over a 32-byte digest with a SEC1 public
 * key. Returns 0 if authentic, -EPERM if not, -EIO on a crypto fault.
 */
static int ecdsa_verify(const uint8_t *pubkey, size_t pubkey_len,
			const uint8_t digest[32], const uint8_t *sig,
			size_t sig_len)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;

	psa_set_key_type(&attr,
			 PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_status_t st = psa_import_key(&attr, pubkey, pubkey_len, &key);

	if (st != PSA_SUCCESS) {
		LOG_ERR("pubkey import failed: %d", (int)st);
		return -EIO;
	}

	st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, 32,
			     sig, sig_len);
	psa_destroy_key(key);

	return (st == PSA_SUCCESS) ? 0 : -EPERM;
}

/*
 * Reject an image that cannot be a valid 32-bit relocatable ELF before the
 * loader touches it: enough bytes for a header, the ELF magic, an ET_REL type,
 * and a section table that lies inside the image. @p hdr supplies at least the
 * ELF header; @p hdr_len is how many header bytes are available; @p total_len
 * is the full image size the section table must lie within (equal to @p hdr_len
 * for a buffer fully in memory, or the file size when streaming).
 */
static int elf_bounds_chk(const uint8_t *hdr, size_t hdr_len, size_t total_len)
{
	if (hdr_len < ELF_HDR_MIN || total_len < ELF_HDR_MIN) {
		return -E2BIG;
	}
	if (sys_get_le32(hdr) != ELF_MAGIC) {
		return -E2BIG;
	}
	if (sys_get_le16(hdr + ELF_E_TYPE_OFF) != ELF_ET_REL) {
		return -E2BIG;
	}

	uint32_t shoff = sys_get_le32(hdr + ELF_E_SHOFF_OFF);
	uint16_t shnum = sys_get_le16(hdr + ELF_E_SHNUM_OFF);
	/* 32-bit section header entry is 40 bytes. */
	uint64_t sh_end = (uint64_t)shoff + (uint64_t)shnum * 40U;

	if (shoff >= total_len || sh_end > total_len) {
		return -E2BIG;
	}
	return 0;
}

/* Bounds check for an image fully held in a memory buffer. */
static int elf_bounds_ok(const uint8_t *buf, size_t len)
{
	return elf_bounds_chk(buf, len, len);
}

/*
 * Set of extensions this loader verified. The launch path checks membership so
 * an unverified extension cannot be run even if a caller hands one in. Sized to
 * the number of concurrent applications the platform supports.
 */
#define VERIFIED_MAX CONFIG_APP_PLATFORM_MAX_APPS
static const struct llext *verified[VERIFIED_MAX];
static K_MUTEX_DEFINE(verified_lock);

static void verified_add(const struct llext *ext)
{
	bool placed = false;

	k_mutex_lock(&verified_lock, K_FOREVER);
	for (int i = 0; i < VERIFIED_MAX; i++) {
		if (verified[i] == NULL) {
			verified[i] = ext;
			placed = true;
			break;
		}
	}
	k_mutex_unlock(&verified_lock);

	if (!placed) {
		LOG_WRN("verified set full; call app_loader_forget on unload");
	}
}

bool app_loader_is_verified(const struct llext *ext)
{
	bool found = false;

	if (ext == NULL) {
		return false;
	}
	k_mutex_lock(&verified_lock, K_FOREVER);
	for (int i = 0; i < VERIFIED_MAX; i++) {
		if (verified[i] == ext) {
			found = true;
			break;
		}
	}
	k_mutex_unlock(&verified_lock);
	return found;
}

void app_loader_forget(const struct llext *ext)
{
	k_mutex_lock(&verified_lock, K_FOREVER);
	for (int i = 0; i < VERIFIED_MAX; i++) {
		if (verified[i] == ext) {
			verified[i] = NULL;
			break;
		}
	}
	k_mutex_unlock(&verified_lock);
}

/*
 * Resolve the entry symbol and enforce the per-application memory budget on a
 * freshly loaded extension. A module whose footprint exceeds the budget is
 * unloaded and rejected, so one module cannot exhaust the shared loadable-
 * extension heap. On success the extension is recorded as verified.
 */
static int finish_load(const char *name, struct llext *loaded,
		       const char *entry_sym, struct llext **ext, void **entry)
{
	void *fn = llext_find_sym(&loaded->exp_tab, entry_sym);

	if (fn == NULL) {
		LOG_ERR("no '%s' entry in '%s'", entry_sym, name);
		llext_unload(&loaded);
		return -ENOENT;
	}

	size_t budget = CONFIG_APP_PLATFORM_LOADER_MAX_MODULE_BYTES;

	if (budget != 0 && loaded->alloc_size > budget) {
		LOG_WRN("'%s' rejected: %zu bytes over %zu-byte budget",
			name, loaded->alloc_size, budget);
		llext_unload(&loaded);
		return -EFBIG;
	}

	/* Reached only after the caller's signature/hash check passed. */
	verified_add(loaded);

	*ext = loaded;
	*entry = fn;
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

	return finish_load(name, loaded, entry_sym, ext, entry);
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

	int rc = ecdsa_verify(pubkey, pubkey_len, digest, sig, sig_len);

	if (rc != 0) {
		LOG_WRN("verify failed for '%s' - bad signature, rejected", name);
		return rc;
	}
	LOG_INF("verify OK for '%s' (ecdsa-p256 signature authentic)", name);

	return load_checked(name, buf, len, entry_sym, ext, entry);
}

int app_loader_verify_load_manifest(const struct app_manifest *mf,
				    const uint8_t *sig, size_t sig_len,
				    const uint8_t *buf, size_t len,
				    const uint8_t *pubkey, size_t pubkey_len,
				    const char *entry_sym, struct llext **ext,
				    void **entry)
{
	uint8_t mf_digest[32];
	uint8_t mod_digest[32];

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	/* 1. The signature must cover this manifest. Authenticating the
	 * manifest authenticates the name and version it carries.
	 */
	if (sha256((const uint8_t *)mf, sizeof(*mf), mf_digest) != 0) {
		return -EIO;
	}
	if (ecdsa_verify(pubkey, pubkey_len, mf_digest, sig, sig_len) != 0) {
		LOG_WRN("manifest signature invalid, rejected");
		return -EPERM;
	}

	/* 2. The manifest must be well-formed and name a sane module. */
	if (mf->magic != APP_MANIFEST_MAGIC) {
		LOG_WRN("bad manifest magic, rejected");
		return -EPERM;
	}
	if (mf->name[APP_MANIFEST_NAME_MAX - 1] != '\0') {
		LOG_WRN("manifest name not terminated, rejected");
		return -EPERM;
	}
	if (mf->module_len != len) {
		LOG_WRN("'%s' module length %u != signed %u, rejected",
			mf->name, (unsigned int)len, mf->module_len);
		return -EPERM;
	}
	if (elf_bounds_ok(buf, len) != 0) {
		LOG_WRN("'%s' rejected: not a valid relocatable module", mf->name);
		return -E2BIG;
	}

	/* 3. The module bytes must be exactly the ones the manifest binds. */
	if (sha256(buf, len, mod_digest) != 0) {
		return -EIO;
	}
	if (memcmp(mod_digest, mf->module_sha256, 32) != 0) {
		LOG_WRN("'%s' module hash != manifest, rejected", mf->name);
		return -EPERM;
	}

	LOG_INF("verify OK for '%s' v%u (manifest authentic, module bound)",
		mf->name, mf->version);

	return load_checked(mf->name, buf, len, entry_sym, ext, entry);
}

/* Streaming SHA-256 over a file: hashes the whole file in small chunks without
 * holding it in memory, and reports the total length. Also returns the first
 * bytes for the structural ELF check.
 */
#define HASH_CHUNK 256

static int sha256_file(const char *path, uint8_t out[32], size_t *out_len,
		       uint8_t *head, size_t head_cap)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_READ);

	if (rc != 0) {
		return rc;
	}

	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;

	if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		fs_close(&f);
		return -EIO;
	}

	uint8_t chunk[HASH_CHUNK];
	size_t total = 0;
	int ret = 0;

	for (;;) {
		ssize_t n = fs_read(&f, chunk, sizeof(chunk));

		if (n < 0) {
			ret = -EIO;
			break;
		}
		if (n == 0) {
			break;
		}
		if (head != NULL && total < head_cap) {
			size_t c = MIN((size_t)n, head_cap - total);

			memcpy(head + total, chunk, c);
		}
		if (psa_hash_update(&op, chunk, n) != PSA_SUCCESS) {
			ret = -EIO;
			break;
		}
		total += (size_t)n;
	}
	fs_close(&f);

	if (ret != 0) {
		psa_hash_abort(&op);
		return ret;
	}

	size_t olen;

	if (psa_hash_finish(&op, out, 32, &olen) != PSA_SUCCESS || olen != 32) {
		return -EIO;
	}
	*out_len = total;
	return 0;
}

static int fs_load_checked(const char *name, const char *path,
			   const char *entry_sym, struct llext **ext,
			   void **entry)
{
	struct llext_fs_loader fl = LLEXT_FS_LOADER(path);
	struct llext_load_param parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *loaded = NULL;
	int rc = llext_load(&fl.loader, name, &loaded, &parm);

	if (rc != 0) {
		LOG_ERR("llext_load (file) failed for '%s': %d", name, rc);
		return rc;
	}

	return finish_load(name, loaded, entry_sym, ext, entry);
}

int app_loader_verify_load_manifest_file(const struct app_manifest *mf,
					 const uint8_t *sig, size_t sig_len,
					 const char *path, const uint8_t *pubkey,
					 size_t pubkey_len, const char *entry_sym,
					 struct llext **ext, void **entry)
{
	uint8_t mf_digest[32];
	uint8_t mod_digest[32];
	uint8_t head[ELF_HDR_MIN];
	size_t len = 0;

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	/* 1. Authenticate the manifest (and thus the name and version). */
	if (sha256((const uint8_t *)mf, sizeof(*mf), mf_digest) != 0) {
		return -EIO;
	}
	if (ecdsa_verify(pubkey, pubkey_len, mf_digest, sig, sig_len) != 0) {
		LOG_WRN("manifest signature invalid, rejected");
		return -EPERM;
	}
	if (mf->magic != APP_MANIFEST_MAGIC ||
	    mf->name[APP_MANIFEST_NAME_MAX - 1] != '\0') {
		LOG_WRN("malformed manifest, rejected");
		return -EPERM;
	}

	/* 2. Stream the module from flash: hash it and capture its header,
	 * without holding the whole module in RAM.
	 */
	if (sha256_file(path, mod_digest, &len, head, sizeof(head)) != 0) {
		LOG_WRN("'%s' module unreadable, rejected", mf->name);
		return -EIO;
	}
	if (mf->module_len != len) {
		LOG_WRN("'%s' module length %u != signed %u, rejected",
			mf->name, (unsigned int)len, mf->module_len);
		return -EPERM;
	}
	if (elf_bounds_chk(head, MIN(len, sizeof(head)), len) != 0) {
		LOG_WRN("'%s' rejected: not a valid relocatable module", mf->name);
		return -E2BIG;
	}
	if (memcmp(mod_digest, mf->module_sha256, 32) != 0) {
		LOG_WRN("'%s' module hash != manifest, rejected", mf->name);
		return -EPERM;
	}

	LOG_INF("verify OK for '%s' v%u (streamed, manifest authentic, module "
		"bound)", mf->name, mf->version);

	/* 3. Load the extension straight from the same file. */
	return fs_load_checked(mf->name, path, entry_sym, ext, entry);
}
