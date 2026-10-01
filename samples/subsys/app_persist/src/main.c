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
 * version downgrade are both rejected. An installed application this firmware
 * cannot load, such as one stored with a retired manifest format, is removed
 * and the embedded release is installed again.
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

/* The most a loaded application may hold; its manifest requests its own. */
#define APP_CAPS_CEILING (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_RANDOM)

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
	return app_loader_verify_load_manifest_file(mf, mf_len, sig, APP_MANIFEST_SIG_LEN, path,
						    "app_main", ext, entry);
}

static const char *refusal_reason(int rc)
{
	switch (rc) {
	case -ENOTSUP:
		return "retired manifest format";
	case -ENOEXEC:
		return "built for another interface or target";
	case -EPERM:
		return "signature or module check failed";
	case -EBADMSG:
		return "malformed manifest or not the installed image";
	case -EACCES:
		return "older than its record or imports a forbidden symbol";
	case -ENXIO:
		return "signed with a key this firmware does not trust";
	case -ECANCELED:
		return "signed with a revoked key";
	default:
		return "cannot be read or verified";
	}
}

static int install_seed(void)
{
	const struct app_manifest *seed = (const struct app_manifest *)persist_manifest;
	int rc = app_registry_store_blob(APP_NAME, persist_app, ARRAY_SIZE(persist_app));

	if (rc == 0) {
		rc = app_registry_store_manifest(APP_NAME, seed, ARRAY_SIZE(persist_manifest),
						 persist_sig, ARRAY_SIZE(persist_sig));
	}
	if (rc == 0) {
		rc = app_registry_install_versioned(APP_NAME, true, seed->version);
	}
	return rc;
}

/* Authenticate the installed manifest, link the module it binds and check both
 * against the installed record.
 */
static int load_installed(union app_manifest_buf *mb, size_t *mf_len, uint8_t *sig,
			  const char *path, struct llext **ext, void **entry)
{
	struct app_record rec;
	int rc = app_registry_load_manifest(APP_NAME, mb, mf_len, sig, APP_MANIFEST_SIG_LEN);

	if (rc != 0) {
		return rc;
	}
	rc = load_from_flash(&mb->mf, *mf_len, sig, path, ext, entry);
	if (rc != 0) {
		return rc;
	}
	rc = find_record(APP_NAME, &rec) ? app_registry_check(&rec, &mb->mf) : -ENOENT;
	if (rc != 0) {
		app_loader_forget(*ext);
		llext_unload(ext);
	}
	return rc;
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
	struct fs_dirent st;
	struct fs_file_t f;
	uint8_t flipped;
	uint8_t b;
	off_t at;
	int rc;

	if (fs_stat(path, &st) != 0 || st.size == 0U) {
		printk("tamper self-test: skipped (no module file)\n");
		return;
	}
	at = (off_t)(st.size / 2U);

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_RDWR) != 0) {
		printk("tamper self-test: skipped (open failed)\n");
		return;
	}
	rc = fs_seek(&f, at, FS_SEEK_SET);
	if (rc == 0 && fs_read(&f, &b, 1) != 1) {
		rc = -EIO;
	}
	if (rc == 0) {
		flipped = b ^ 0xffU;
		rc = fs_seek(&f, at, FS_SEEK_SET);
	}
	if (rc == 0 && fs_write(&f, &flipped, 1) != 1) {
		rc = -EIO;
	}
	(void)fs_close(&f);
	if (rc != 0) {
		printk("tamper self-test: skipped (rc=%d)\n", rc);
		return;
	}

	rc = load_from_flash(mf, mf_len, sig, path, &ext, &entry);
	if (rc == 0) {
		app_loader_forget(ext);
		llext_unload(&ext);
	}

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_RDWR) != 0) {
		printk("tamper self-test: could not restore the module byte\n");
	} else {
		if (fs_seek(&f, at, FS_SEEK_SET) != 0 || fs_write(&f, &b, 1) != 1) {
			printk("tamper self-test: could not restore the module byte\n");
		}
		(void)fs_close(&f);
	}

	if (rc == -EPERM) {
		printk("tamper self-test: altered module rejected (rc=%d) OK\n", rc);
	} else {
		printk("SECURITY FAIL: altered module not refused by its hash (rc=%d)\n", rc);
	}
}

int main(void)
{
	union app_manifest_buf mb;
	const struct app_manifest *mf = &mb.mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	struct app_record rec;
	struct llext *ext = NULL;
	void *entry = NULL;
	size_t mf_len = 0;
	char path[64];
	int rc;

	printk("\n*** persistent signed loadable app (C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	if (app_registry_load() != 0) {
		printk("registry load failed\n");
		return 0;
	}
	if (app_registry_blob_path(APP_NAME, path, sizeof(path)) != 0) {
		printk("could not build '%s' binary path\n", APP_NAME);
		return 0;
	}

	if (!find_record(APP_NAME, &rec)) {
		printk("first boot: installing signed '%s' to flash\n", APP_NAME);
		rc = install_seed();
		if (rc != 0) {
			printk("install failed (rc=%d)\n", rc);
			return 0;
		}
		printk("installed and persisted to flash\n");
	} else {
		printk("found '%s' already installed - restored after reboot\n", APP_NAME);
	}

	rc = load_installed(&mb, &mf_len, sig, path, &ext, &entry);
	if (rc != 0) {
		printk("installed '%s' cannot run on this firmware: %s (rc=%d)\n", APP_NAME,
		       refusal_reason(rc), rc);
		printk("removing it and installing the bundled release\n");
		(void)app_registry_uninstall(APP_NAME);
		rc = install_seed();
		if (rc != 0) {
			printk("install failed (rc=%d)\n", rc);
			return 0;
		}
		rc = load_installed(&mb, &mf_len, sig, path, &ext, &entry);
		if (rc != 0) {
			printk("bundled release did not load: %s (rc=%d)\n", refusal_reason(rc),
			       rc);
			return 0;
		}
	}
	printk("verified '%s' v%u from %s (%u bytes)\n", mf->name, mf->version, path,
	       mf->module_len);

	/* Anti-downgrade: offering an older version than the installed record
	 * must be refused without changing the stored record.
	 */
	rc = app_registry_install_versioned(APP_NAME, true, 0);
	if (rc == -EACCES) {
		printk("rollback self-test: downgrade to v0 refused OK\n");
	} else {
		printk("SECURITY FAIL: downgrade accepted (rc=%d)\n", rc);
	}

	tamper_self_test(mf, mf_len, sig, path);

	rc = app_platform_launch_llext(mf->name, ext, (app_entry_t)entry, NULL, APP_CAPS_CEILING);
	if (rc < 0) {
		printk("launch failed (rc=%d)\n", rc);
		return 0;
	}
	printk("running authenticated '%s' v%u from persisted binary\n", mf->name, mf->version);

	for (;;) {
		k_msleep(5000);
		printk("*** '%s' still running; reboot and it returns from "
		       "flash ***\n", mf->name);
	}
	return 0;
}
