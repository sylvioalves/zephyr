/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Persistent signed loadable application with an authenticated identity.
 *
 * The download is a signed manifest plus the module it binds. The manifest
 * carries the application's name and version and the module's hash, and the
 * ECDSA P-256 signature covers the manifest - so the name and version are
 * authenticated, not asserted, and a valid signature cannot be moved onto
 * different bytes or relabelled. On the first boot the embedded release is
 * written to the storage partition and recorded in the registry at the
 * manifest's version, so a later downgrade is refused. On every boot the
 * manifest is re-verified, the module is accepted only if it matches the signed
 * hash, and only then is it launched. After a reset it returns from flash
 * without being downloaded again. Self-tests show a tampered module and a
 * version downgrade are both rejected.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fs/fs.h>
#include <string.h>
#include <errno.h>

/* The "downloaded" release: module bytes, the signed manifest that binds the
 * name and version to those bytes, its signature, and the trusted public key.
 * The private key never reaches the device; only this public key does.
 */
static const uint8_t persist_app[] = {
#include "persist.inc"
};
static const uint8_t persist_manifest[] = {
#include "persist.manifest.inc"
};
static const uint8_t persist_sig[] = {
#include "persist.sig.inc"
};
static const uint8_t persist_pubkey[] = {
#include "persist.pubkey.inc"
};

#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME)

static bool find_record(const char *name, struct app_record *out)
{
	for (int i = 0; i < app_registry_count(); i++) {
		if (app_registry_get(i, out) == 0 && strcmp(out->name, name) == 0) {
			return true;
		}
	}
	return false;
}

static int load_from_flash(const struct app_manifest *mf, size_t mf_len, const uint8_t *sig,
			   const char *path, struct llext **ext, void **entry)
{
	return app_loader_verify_load_manifest_file(mf, mf_len, sig, APP_MANIFEST_SIG_LEN,
						    path, persist_pubkey,
						    ARRAY_SIZE(persist_pubkey),
						    "app_main", ext, entry);
}

/* Prove the binding without holding the module in RAM: flip one byte in the
 * on-flash module file and confirm the streamed load is refused because the
 * module no longer matches the signed manifest hash, then restore the byte.
 */
static void tamper_self_test(const struct app_manifest *mf, size_t mf_len, const uint8_t *sig,
			     const char *path)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	struct fs_file_t f;
	uint8_t b;
	off_t at = mf->module_len / 2;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_RDWR) != 0) {
		printk("tamper self-test: skipped (open failed)\n");
		return;
	}
	fs_seek(&f, at, FS_SEEK_SET);
	fs_read(&f, &b, 1);
	uint8_t flipped = b ^ 0xff;

	fs_seek(&f, at, FS_SEEK_SET);
	fs_write(&f, &flipped, 1);
	fs_close(&f);

	int rc = load_from_flash(mf, mf_len, sig, path, &ext, &entry);

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_RDWR) == 0) {
		fs_seek(&f, at, FS_SEEK_SET);
		fs_write(&f, &b, 1);
		fs_close(&f);
	}

	if (rc == 0) {
		printk("SECURITY FAIL: tampered module was accepted!\n");
	} else {
		printk("tamper self-test: altered module rejected (rc=%d) OK\n",
		       rc);
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

	struct app_record rec;

	if (!find_record(APP_NAME, &rec)) {
		printk("first boot: installing signed '%s' to flash\n", APP_NAME);
		const struct app_manifest *seed =
			(const struct app_manifest *)persist_manifest;
		int rc = app_registry_store_blob(APP_NAME, persist_app,
						 ARRAY_SIZE(persist_app));

		if (rc == 0) {
			rc = app_registry_store_manifest(APP_NAME, seed,
							 ARRAY_SIZE(persist_manifest),
							 persist_sig,
							 ARRAY_SIZE(persist_sig));
		}
		if (rc == 0) {
			/* Version comes from the authenticated manifest. */
			rc = app_registry_install_versioned(APP_NAME, true,
							    seed->version);
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

	/* Load the manifest+signature from flash and authenticate them. */
	union app_manifest_buf mb;
	const struct app_manifest *mf = &mb.mf;
	size_t mf_len = 0;
	uint8_t sig[APP_MANIFEST_SIG_LEN];

	if (app_registry_load_manifest(APP_NAME, &mb, &mf_len, sig, sizeof(sig)) != 0) {
		printk("could not read '%s' manifest from flash\n", APP_NAME);
		return 0;
	}

	/* Anti-downgrade: offering an older version than the installed record
	 * must be refused without changing the stored record.
	 */
	int down = app_registry_install_versioned(APP_NAME, true, 0);

	if (down == -EACCES) {
		printk("rollback self-test: downgrade to v0 refused OK\n");
	} else {
		printk("SECURITY FAIL: downgrade accepted (rc=%d)\n", down);
	}

	char path[64];

	if (app_registry_blob_path(APP_NAME, path, sizeof(path)) != 0) {
		printk("could not build '%s' binary path\n", APP_NAME);
		return 0;
	}
	printk("streaming '%s' v%u from %s (%u bytes)\n", mf->name, mf->version,
	       path, mf->module_len);

	tamper_self_test(mf, mf_len, sig, path);

	struct llext *ext = NULL;
	void *entry = NULL;

	if (load_from_flash(mf, mf_len, sig, path, &ext, &entry) != 0) {
		printk("manifest verify/load of flash binary failed\n");
		return 0;
	}
	if (!find_record(APP_NAME, &rec) || app_registry_check(&rec, mf) != 0) {
		printk("'%s' on flash does not match its installed record\n", APP_NAME);
		app_loader_forget(ext);
		llext_unload(&ext);
		return 0;
	}

	app_platform_launch_llext(mf->name, ext, (app_entry_t)entry, NULL, APP_CAPS);
	printk("running authenticated '%s' v%u from persisted binary\n",
	       mf->name, mf->version);

	for (;;) {
		k_msleep(5000);
		printk("*** '%s' still running; reboot and it returns from "
		       "flash ***\n", mf->name);
	}
	return 0;
}
