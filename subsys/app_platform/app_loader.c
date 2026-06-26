/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Verify a module by SHA-256 and load it as an llext. The hash is checked before
 * the module is loaded, so a tampered or corrupted module is rejected and never
 * linked - the install path a network-downloaded module would follow.
 */

#include <zephyr/app_platform/app_loader.h>

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <zephyr/logging/log.h>
#include <psa/crypto.h>
#include <string.h>

LOG_MODULE_DECLARE(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

static int sha256(const uint8_t *buf, size_t len, uint8_t out[32])
{
	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, buf, len, out, 32, &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
}

int app_loader_verify_load(const char *name, const uint8_t *buf, size_t len,
			   const uint8_t expect[32], const char *entry_sym,
			   struct llext **ext, void **entry)
{
	uint8_t got[32];

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	if (sha256(buf, len, got) != 0) {
		return -EIO;
	}
	if (memcmp(got, expect, 32) != 0) {
		LOG_WRN("verify failed for '%s' - hash mismatch, rejected", name);
		return -EPERM;
	}
	LOG_INF("verify OK for '%s' (sha-256 matches)", name);

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
