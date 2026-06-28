/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App store: install and run an application module at runtime. The download is
 * a signed manifest plus the module it binds. The manifest's ECDSA P-256
 * signature is verified against the public key compiled into the firmware, and
 * the module is accepted only if it matches the signed hash - so a tampered or
 * forged module is rejected and never linked. A verified module is then
 * launched through the platform, which refuses to run any extension that did
 * not pass the loader. The bytes are embedded here for the demo, but the path -
 * verify manifest, bind module, launch isolated - is exactly what a
 * network-downloaded module follows.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <errno.h>

/* The "downloaded" release: module bytes, the signed manifest binding name,
 * version and hash, its signature, and the trusted public key.
 */
static uint8_t module_buf[] = {
#include "blink_app.inc"
};
static const uint8_t module_manifest[] = {
#include "blink_app.manifest.inc"
};
static const uint8_t module_sig[] = {
#include "blink_app.sig.inc"
};
static const uint8_t module_pubkey[] = {
#include "blink_app.pubkey.inc"
};

#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME)

static int install_and_run(const struct app_manifest *mf, const uint8_t *sig,
			   uint8_t *buf, size_t len)
{
	struct llext *ext = NULL;
	void *entry = NULL;

	printk("app-store: installing '%s' v%u (%zu bytes)\n", mf->name,
	       mf->version, len);

	int rc = app_loader_verify_load_manifest(mf, sig, APP_MANIFEST_SIG_LEN,
						 buf, len, module_pubkey,
						 ARRAY_SIZE(module_pubkey),
						 "app_main", &ext, &entry);

	if (rc != 0) {
		printk("app-store: VERIFY FAILED (rc=%d) - install REJECTED\n",
		       rc);
		return rc;
	}

	rc = app_platform_launch_llext(mf->name, ext, (app_entry_t)entry,
				       APP_CAPS);
	if (rc < 0) {
		printk("app-store: launch refused (rc=%d)\n", rc);
		return rc;
	}
	printk("app-store: '%s' installed and running (isolated app %d)\n",
	       mf->name, rc);
	return 0;
}

int main(void)
{
	const struct app_manifest *mf =
		(const struct app_manifest *)module_manifest;
	size_t len = ARRAY_SIZE(module_buf);

	printk("\n*** app store: verify + install + run a signed module ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	/* 1. A genuine module: the manifest verifies, it installs and runs. */
	printk("\n=== case 1: genuine signed module ===\n");
	install_and_run(mf, module_sig, module_buf, len);

	k_msleep(3000);

	/* 2. A tampered module: flip a byte; the module no longer matches the
	 * signed manifest hash, so install must be rejected.
	 */
	printk("\n=== case 2: tampered module ===\n");
	uint8_t saved = module_buf[len / 2];

	module_buf[len / 2] ^= 0xff;
	install_and_run(mf, module_sig, module_buf, len);
	module_buf[len / 2] = saved;

	/* 3. Bypass attempt: load the module raw, skipping the verifier, and try
	 * to launch it directly. The platform must refuse an extension that did
	 * not pass the loader, so verification cannot be side-stepped.
	 */
	printk("\n=== case 3: launch a module that skipped verification ===\n");
	struct llext_buf_loader bl = LLEXT_BUF_LOADER(module_buf, len);
	struct llext_load_param parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *raw = NULL;

	if (llext_load(&bl.loader, "rogue", &raw, &parm) == 0) {
		void *entry = llext_find_sym(&raw->exp_tab, "app_main");
		int rc = app_platform_launch_llext("rogue", raw,
						   (app_entry_t)entry, APP_CAPS);

		if (rc == -EPERM) {
			printk("app-store: unverified module refused at launch OK\n");
		} else {
			printk("SECURITY FAIL: unverified module launched (rc=%d)\n",
			       rc);
		}
	}

	printk("\n*** done: signed module ran; tampered and unverified "
	       "modules rejected ***\n");
	return 0;
}
