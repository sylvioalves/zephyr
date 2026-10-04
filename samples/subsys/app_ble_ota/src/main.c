/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Updating an application over BLE, for a device with no network connection.
 *
 * A peer board hosts one signed application (see the peer/ directory). This
 * device runs a fetcher application that holds APP_CAP_BLE and asks the
 * platform to pull the image; the platform writes the manifest, signature and
 * module straight to flash without the fetcher ever seeing them.
 *
 * The supervisor then verifies the signature against the public key compiled
 * in here, installs the application in the registry and launches it. The
 * transport is untrusted throughout: a peer that serves tampered bytes, an
 * unsigned image or a manifest naming a different application is rejected by
 * the same check an HTTP download goes through.
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

static const uint8_t fetcher_app[] __aligned(8) = {
#include "fetcher_app.inc"
};
static const uint8_t fetcher_hash[32] = {
#include "fetcher_app.sha256.inc"
};

/*
 * The fetcher needs BLE and nothing else. FETCHED_CAPS is the most the
 * application that arrives may hold, whatever its manifest requests, and
 * notably not BLE: what was updated over the air cannot itself reach the peer.
 */
#define FETCHER_CAPS (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_BLE)
#define FETCHED_CAPS (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_RANDOM)

#define APP_NAME "weather"

/* Verify what the transport staged, then install and launch it. */
static int install_fetched(const char *name)
{
	union app_manifest_buf mb;
	const struct app_manifest *mf = &mb.mf;
	size_t mf_len = 0;
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	char path[64];

	if (app_registry_load_staged_manifest(name, &mb, &mf_len, sig, sizeof(sig)) != 0) {
		printk("no manifest staged for '%s'\n", name);
		app_registry_discard(name);
		return -ENOENT;
	}
	if (app_registry_staged_blob_path(name, path, sizeof(path)) != 0) {
		return -ENAMETOOLONG;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int64_t t0 = k_uptime_get();
	int rc = app_loader_verify_load_manifest_file(mf, mf_len, sig, APP_MANIFEST_SIG_LEN, path,
						      "app_main", &ext, &entry);

	if (rc != 0) {
		printk("verify/load failed (%d) - update rejected\n", rc);
		app_registry_discard(name);
		return rc;
	}
	printk("verified '%s' v%u in %lld ms\n", mf->name, mf->version,
	       k_uptime_get() - t0);

	rc = app_registry_commit(name, true, mf);
	if (rc != 0) {
		printk("install refused (%d)\n", rc);
		app_loader_forget(ext);
		llext_unload(&ext);
		return rc;
	}

	rc = app_platform_launch_llext(mf->name, ext, (app_entry_t)entry, NULL,
				       FETCHED_CAPS);
	if (rc < 0) {
		printk("launch refused (%d)\n", rc);
		return rc;
	}
	printk("updated over BLE: '%s' v%u running as app %d\n", mf->name,
	       mf->version, rc);
	return 0;
}

int main(void)
{
	printk("\n*** application update over BLE ***\n");

	if (app_platform_init() != 0 || app_registry_load() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("fetcher", fetcher_app,
					ARRAY_SIZE(fetcher_app), fetcher_hash,
					"app_main", &ext, &entry);

	if (rc == 0) {
		rc = app_platform_launch_llext("fetcher", ext,
					       (app_entry_t)entry, NULL, FETCHER_CAPS);
	}
	if (rc < 0) {
		printk("fetcher app failed to start (%d)\n", rc);
		return 0;
	}
	int fetcher_id = rc;

	printk("fetcher running as app %d; pulling '%s' from the peer...\n",
	       rc, APP_NAME);

	/* Wait for this run's transfer to finish: any staged leftover is
	 * dropped first, so the staged file reappearing means the fetch ran.
	 */
	char path[64];

	if (app_registry_staged_blob_path(APP_NAME, path, sizeof(path)) != 0) {
		return 0;
	}
	app_registry_discard(APP_NAME);

	for (int i = 0; i < 60; i++) {
		struct fs_dirent st;

		k_msleep(1000);
		if (fs_stat(path, &st) != 0 || st.size == 0) {
			continue;
		}
		/* Present and no longer growing: the transfer is done. */
		off_t len = st.size;

		k_msleep(1000);
		if (fs_stat(path, &st) == 0 && st.size == len) {
			/* The fetcher has done its job. Reclaim it before the
			 * new application is loaded so the update does not
			 * need room for both at once.
			 */
			(void)app_platform_remove(fetcher_id);
			k_msleep(200);
			(void)install_fetched(APP_NAME);
			return 0;
		}
	}
	printk("no image arrived from the peer\n");
	return 0;
}
