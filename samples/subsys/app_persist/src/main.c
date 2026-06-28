/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Persistent signed loadable application: an authenticated app survives a
 * reboot.
 *
 * On the first boot the embedded module bytes stand in for a downloaded
 * release. They are written to the storage partition and recorded in the
 * registry with a version, so a later downgrade is refused. On every boot -
 * first or later - the app is loaded from flash (not from the embedded copy),
 * its ECDSA P-256 signature is verified against the public key compiled into
 * the firmware, and only then is it linked and launched. After a reset the
 * binary is still on flash, so it returns without being downloaded again. A
 * tamper self-test flips one byte and shows the signed loader rejects it.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>

/* The "downloaded" release: module bytes, its signature, and the trusted
 * public key. The private key never reaches the device; only this public key
 * does, so only an image signed by the build host is accepted.
 */
static const uint8_t persist_app[] = {
#include "persist.inc"
};
static const uint8_t persist_sig[] = {
#include "persist.sig.inc"
};
static const uint8_t persist_pubkey[] = {
#include "persist.pubkey.inc"
};

#define APP_NAME    "persist"
#define APP_VERSION 1
#define APP_CAPS    (APP_CAP_LOG | APP_CAP_TIME)

static uint8_t load_buf[8192];

static bool is_installed(const char *name)
{
	for (int i = 0; i < app_registry_count(); i++) {
		struct app_record r;

		if (app_registry_get(i, &r) == 0 && strcmp(r.name, name) == 0) {
			return true;
		}
	}
	return false;
}

static int load_signed(const char *name, uint8_t *buf, size_t len,
		       struct llext **ext, void **entry)
{
	return app_loader_verify_load_signed(name, buf, len, persist_sig,
					     ARRAY_SIZE(persist_sig),
					     persist_pubkey,
					     ARRAY_SIZE(persist_pubkey),
					     "app_main", ext, entry);
}

/* Prove the signed loader rejects a forged image: flip one byte and confirm
 * the load is refused, then restore the buffer.
 */
static void tamper_self_test(uint8_t *buf, size_t len)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	uint8_t saved = buf[len / 2];

	buf[len / 2] ^= 0xff;
	int rc = load_signed(APP_NAME "_tampered", buf, len, &ext, &entry);

	buf[len / 2] = saved;

	if (rc == 0) {
		printk("SECURITY FAIL: tampered image was accepted!\n");
		if (ext != NULL) {
			(void)entry;
		}
	} else {
		printk("tamper self-test: forged image rejected (rc=%d) OK\n", rc);
	}
}

int main(void)
{
	printk("\n*** persistent signed loadable app (C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	if (app_registry_load() != 0) {
		printk("registry load failed\n");
		return 0;
	}

	if (!is_installed(APP_NAME)) {
		printk("first boot: installing signed '%s' v%d to flash\n",
		       APP_NAME, APP_VERSION);
		int rc = app_registry_store_blob(APP_NAME, persist_app,
						 ARRAY_SIZE(persist_app));

		if (rc == 0) {
			rc = app_registry_install_versioned(APP_NAME, true,
							    APP_VERSION);
		}
		if (rc != 0) {
			printk("install failed (rc=%d)\n", rc);
			return 0;
		}
		printk("installed and persisted to flash\n");
	} else {
		printk("found '%s' already installed - restored after reboot\n",
		       APP_NAME);
	}

	/* Anti-downgrade: a signed-but-older image must be refused once a newer
	 * one has been accepted. The installed record is v1; offering v0 must
	 * fail with -EACCES without changing the stored record.
	 */
	int down = app_registry_install_versioned(APP_NAME, true, 0);

	if (down == -EACCES) {
		printk("rollback self-test: downgrade to v0 refused OK\n");
	} else {
		printk("SECURITY FAIL: downgrade accepted (rc=%d)\n", down);
	}

	size_t len = 0;

	if (app_registry_load_blob(APP_NAME, load_buf, sizeof(load_buf),
				   &len) != 0) {
		printk("could not read '%s' binary from flash\n", APP_NAME);
		return 0;
	}
	printk("loaded '%s' from flash (%zu bytes)\n", APP_NAME, len);

	tamper_self_test(load_buf, len);

	struct llext *ext = NULL;
	void *entry = NULL;

	if (load_signed(APP_NAME, load_buf, len, &ext, &entry) != 0) {
		printk("signature verify/load of flash binary failed\n");
		return 0;
	}

	app_platform_launch_llext(APP_NAME, ext, (app_entry_t)entry, APP_CAPS);
	printk("running authenticated '%s' from persisted binary\n", APP_NAME);

	for (;;) {
		k_msleep(5000);
		printk("*** '%s' still running; reboot and it returns from "
		       "flash ***\n", APP_NAME);
	}
	return 0;
}
