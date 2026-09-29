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
