/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>

#include "module_run.h"

static char watched[32];
static atomic_t watched_faulted;

static void watch_reclaimed(int id)
{
	struct app_platform_status st;

	if (app_platform_status(id, &st) == 0 && st.faulted && st.name != NULL &&
	    strncmp(st.name, watched, sizeof(watched)) == 0) {
		atomic_set(&watched_faulted, 1);
	}
}

void module_watch(const char *name)
{
	static bool hooked;

	if (!hooked) {
		zassert_equal(app_platform_on_reclaim(watch_reclaimed), 0);
		hooked = true;
	}
	strncpy(watched, name, sizeof(watched) - 1U);
	atomic_clear(&watched_faulted);
}

bool module_faulted(void)
{
	return atomic_get(&watched_faulted) != 0;
}

static bool running(int id)
{
	struct app_platform_status st;

	return app_platform_status(id, &st) == 0 && st.name != NULL &&
	       strncmp(st.name, watched, sizeof(watched)) == 0;
}

int module_wait_end(int id, int ms)
{
	for (int i = 0; i < ms / 5 && running(id); i++) {
		k_msleep(5);
	}
	if (running(id)) {
		(void)app_platform_remove(id);
		for (int i = 0; i < 400 && running(id); i++) {
			k_msleep(5);
		}
		return -ETIMEDOUT;
	}
	return module_faulted() ? -EFAULT : 0;
}
