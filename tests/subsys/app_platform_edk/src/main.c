/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Modules built and signed outside the tree with the development kit of this
 * firmware and with the kit of another firmware for the same target, one with
 * other syscalls and another tick rate, run in this firmware.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/llext/llext.h>
#include <zephyr/kernel.h>
#include <zephyr/syscall_list.h>
#include <string.h>

#include "edk/own_facts.h"
#include "edk/peer_facts.h"

static const uint8_t own_mod[] = {
#include "edk/own_hello.llext.inc"
};
static const uint8_t own_bundle[] = {
#include "edk/own_hello.manifest.inc"
};
static const uint8_t peer_mod[] = {
#include "edk/peer_hello.llext.inc"
};
static const uint8_t peer_bundle[] = {
#include "edk/peer_hello.manifest.inc"
};
static const uint8_t unlisted_mod[] = {
#include "edk/own_unlisted.llext.inc"
};
static const uint8_t unlisted_bundle[] = {
#include "edk/own_unlisted.manifest.inc"
};

static uint8_t work[8192] __aligned(8);

static atomic_t app_faults;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	if (!app_platform_handle_fatal_error(reason, esf)) {
		k_fatal_halt(reason);
	}
	atomic_inc(&app_faults);
}

static int slot_of(const char *name)
{
	for (int i = 0; i < app_platform_count(); i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0 && st.name != NULL &&
		    strcmp(st.name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool wait_slot_free(const char *name, int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		if (slot_of(name) < 0) {
			return true;
		}
		k_msleep(10);
	}
	return slot_of(name) < 0;
}

static int load(const uint8_t *mod, size_t mod_len, const uint8_t *bundle, size_t bundle_len,
		struct app_manifest *mf, struct llext **ext, void **entry)
{
	size_t mf_len = bundle_len - APP_MANIFEST_SIG_LEN;

	zassert_true(mod_len <= sizeof(work), "module too big");
	zassert_equal(mf_len, sizeof(*mf), "manifest length %zu", mf_len);
	memcpy(work, mod, mod_len);
	memcpy(mf, bundle, mf_len);
	return app_loader_verify_load_manifest(mf, mf_len, bundle + mf_len, APP_MANIFEST_SIG_LEN,
					       work, mod_len, "app_main", ext, entry);
}

static void run(const uint8_t *mod, size_t mod_len, const uint8_t *bundle, size_t bundle_len)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	int id;

	zassert_equal(load(mod, mod_len, bundle, bundle_len, &mf, &ext, &entry), 0);
	zassert_equal(mf.key_id, 1U, "signed with key id %u", mf.key_id);

	id = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, NULL,
				       APP_CAP_LOG | APP_CAP_TIME);
	zassert_true(id >= 0, "launch rc %d", id);
	if (!wait_slot_free(mf.name, 5000)) {
		(void)app_platform_remove(id);
		zassert_true(wait_slot_free(mf.name, 2000), "the module was not removed");
		zassert_unreachable("the module did not measure the sleep");
	}
	zassert_equal(atomic_get(&app_faults), 0, "the module faulted");
}

ZTEST(app_edk, test_firmware_differs)
{
	zassert_equal(CONFIG_SYS_CLOCK_TICKS_PER_SEC, OWN_TICKS_PER_SEC);
	zassert_equal(K_SYSCALL_APP_SVC_UPTIME_MS, OWN_SYSCALL_APP_SVC_UPTIME_MS);
	zassert_not_equal(OWN_TICKS_PER_SEC, PEER_TICKS_PER_SEC,
			  "the two firmwares have the same tick rate");
	zassert_not_equal(OWN_SYSCALL_APP_SVC_UPTIME_MS, PEER_SYSCALL_APP_SVC_UPTIME_MS,
			  "the service syscalls of the two firmwares have the same numbers");
}

ZTEST(app_edk, test_kits_build_the_same_module)
{
	zassert_equal(sizeof(own_mod), sizeof(peer_mod));
	zassert_mem_equal(own_mod, peer_mod, sizeof(own_mod),
			  "the kits of the two firmwares build different modules");
}

ZTEST(app_edk, test_own_kit_module_runs)
{
	run(own_mod, sizeof(own_mod), own_bundle, sizeof(own_bundle));
}

ZTEST(app_edk, test_peer_kit_module_runs)
{
	run(peer_mod, sizeof(peer_mod), peer_bundle, sizeof(peer_bundle));
}

ZTEST(app_edk, test_unlisted_key_refused)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;

	zassert_equal(load(unlisted_mod, sizeof(unlisted_mod), unlisted_bundle,
			   sizeof(unlisted_bundle), &mf, &ext, &entry),
		      -ENXIO);
	zassert_equal(mf.key_id, 7U);
	zassert_is_null(ext);
}

static void *edk_setup(void)
{
	zassert_equal(app_platform_init(), 0, "platform init failed");
	return NULL;
}

ZTEST_SUITE(app_edk, NULL, edk_setup, NULL, NULL, NULL);
