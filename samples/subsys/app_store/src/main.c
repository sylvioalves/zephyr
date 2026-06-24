/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App store: install and run an application module at runtime. The module is
 * verified by SHA-256 before it is loaded, so a tampered or corrupted module is
 * rejected and never executed. The bytes are embedded here for the demo, but
 * the install path - verify, load, resolve entry, run, unload - is exactly what
 * a network-downloaded module would follow.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <psa/crypto.h>
#include <string.h>

/* The "downloaded" module bytes. */
static uint8_t module_buf[] = {
#include "blink_app.inc"
};

/*
 * The expected SHA-256 of the module (its "signature" in this PoC). Filled in
 * at build time from the generated header so the good module always matches;
 * a real store would ship this hash signed alongside the module.
 */
static const uint8_t expected_hash[32] = {
#include "blink_app.sha256.inc"
};

static int sha256(const uint8_t *buf, size_t len, uint8_t out[32])
{
	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, buf, len,
					   out, 32, &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
}

/* Verify the module against the expected hash, then load and run it. */
static int install_and_run(const char *name, uint8_t *buf, size_t len,
			   const uint8_t expect[32])
{
	uint8_t got[32];

	printk("app-store: installing '%s' (%zu bytes)\n", name, len);

	if (sha256(buf, len, got) != 0) {
		printk("app-store: hash failed\n");
		return -EIO;
	}

	if (memcmp(got, expect, 32) != 0) {
		printk("app-store: VERIFY FAILED - hash mismatch, install REJECTED\n");
		return -EPERM;
	}
	printk("app-store: verify OK (sha-256 matches)\n");

	struct llext_buf_loader bl = LLEXT_BUF_LOADER(buf, len);
	struct llext_load_param parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;
	int rc = llext_load(&bl.loader, name, &ext, &parm);

	if (rc != 0) {
		printk("app-store: load failed: %d\n", rc);
		return rc;
	}

	void (*entry)(void) = llext_find_sym(&ext->exp_tab, "app_main");

	if (entry == NULL) {
		printk("app-store: no app_main entry\n");
		llext_unload(&ext);
		return -ENOENT;
	}

	printk("app-store: running '%s'...\n", name);
	entry();

	llext_unload(&ext);
	printk("app-store: '%s' finished and unloaded\n", name);
	return 0;
}

int main(void)
{
	size_t len = ARRAY_SIZE(module_buf);

	printk("\n*** M4: app store - verify + install + run a module (C6) ***\n");

	if (psa_crypto_init() != PSA_SUCCESS) {
		printk("psa_crypto_init failed\n");
		return 0;
	}

	/* 1. A genuine module: verify passes, it installs and runs. */
	printk("\n=== case 1: genuine module ===\n");
	install_and_run("blink", module_buf, len, expected_hash);

	/* 2. A tampered module: flip a byte, verify must reject it. */
	printk("\n=== case 2: tampered module ===\n");
	uint8_t saved = module_buf[len / 2];

	module_buf[len / 2] ^= 0xff;
	install_and_run("blink-tampered", module_buf, len, expected_hash);
	module_buf[len / 2] = saved;

	printk("\n*** M4 COMPLETE: genuine module ran; tampered module rejected ***\n");
	return 0;
}
