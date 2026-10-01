/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A module built and signed by another firmware for the same target, one with
 * other syscalls and another tick rate, runs in this firmware.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/llext/llext.h>
#include <zephyr/kernel.h>
#include <zephyr/syscall_list.h>
#include <string.h>

#include "renumber_probe.h"
#include "producer/renumber_facts.h"

static const uint8_t mod_bytes[] = {
#include "producer/renumber_mod.inc"
};
static const uint8_t mod_manifest[] = {
#include "producer/renumber_mod.manifest.inc"
};
static const uint8_t mod_sig[] = {
#include "producer/renumber_mod.sig.inc"
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

ZTEST(app_abi_renumber, test_firmware_differs)
{
	zassert_equal(aaa_renumber_probe(1), 2);
	zassert_not_equal(K_SYSCALL_APP_SVC_UPTIME_MS, PRODUCER_SYSCALL_APP_SVC_UPTIME_MS,
			  "the service syscalls were not renumbered");
	zassert_not_equal(CONFIG_SYS_CLOCK_TICKS_PER_SEC, PRODUCER_TICKS_PER_SEC,
			  "the tick rate is the producer's");
}

ZTEST(app_abi_renumber, test_foreign_module_runs)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	int id;

	zassert_true(sizeof(mod_bytes) <= sizeof(work), "module too big");
	memcpy(work, mod_bytes, sizeof(mod_bytes));
	memcpy(&mf, mod_manifest, sizeof(mf));
	zassert_equal(app_loader_verify_load_manifest(&mf, sizeof(mf), mod_sig, sizeof(mod_sig),
						      work, sizeof(mod_bytes), "app_main", &ext,
						      &entry),
		      0);

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

static void *renumber_setup(void)
{
	zassert_equal(app_platform_init(), 0, "platform init failed");
	return NULL;
}

ZTEST_SUITE(app_abi_renumber, NULL, renumber_setup, NULL, NULL, NULL);
