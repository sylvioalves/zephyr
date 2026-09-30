/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/app_platform/app_platform.h>

#include "app_grants.h"
#include "locked_svc.h"

static SVC_LOCK_DEFINE(test_lock);
static atomic_t hook_runs;

static SVC_LOCK_DEFINE(hold_lock);
static K_SEM_DEFINE(hold_go, 0, 1);
static atomic_t hold_taken;
static atomic_t psram_fault;

int z_impl_test_svc_locked(int fault)
{
	int rc = app_service_lock(&test_lock, SVC_LOCK_TIMEOUT);

	if (rc != 0) {
		return rc;
	}
	if (fault != 0) {
		k_oops();
	}
	(void)app_service_unlock(&test_lock);
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_test_svc_locked(int fault)
{
	return z_impl_test_svc_locked(fault);
}
#include <zephyr/syscalls/test_svc_locked_mrsh.c>
#endif

static void locked_hook(int id)
{
	ARG_UNUSED(id);

	if (app_service_lock(&test_lock, K_FOREVER) == 0) {
		(void)app_service_unlock(&test_lock);
	}
	atomic_inc(&hook_runs);
}

int test_svc_locked_hook_register(void)
{
	return app_platform_on_reclaim(locked_hook);
}

int test_svc_locked_hook_runs(void)
{
	return (int)atomic_get(&hook_runs);
}

int z_impl_test_svc_hold(int mode)
{
	int rc = app_service_lock(&hold_lock,
				  (mode == TEST_HOLD_WAIT) ? K_FOREVER : SVC_LOCK_TIMEOUT);

	if (rc != 0) {
		return rc;
	}
	if (mode == TEST_HOLD_OWN) {
		atomic_set(&hold_taken, 1);
		(void)k_sem_take(&hold_go, K_FOREVER);
	}
	(void)app_service_unlock(&hold_lock);
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_test_svc_hold(int mode)
{
	return z_impl_test_svc_hold(mode);
}
#include <zephyr/syscalls/test_svc_hold_mrsh.c>
#endif

void test_svc_hold_release(void)
{
	k_sem_give(&hold_go);
}

bool test_svc_hold_taken(void)
{
	return atomic_get(&hold_taken) != 0;
}

void test_psram_fault_arm(void)
{
	atomic_set(&psram_fault, 1);
}

bool test_psram_fault_armed(void)
{
	return atomic_get(&psram_fault) != 0;
}

void app_platform_test_point(int point)
{
	if (point == APP_TEST_POINT_PSRAM_MAP && atomic_cas(&psram_fault, 1, 0)) {
		k_oops();
	}
}
