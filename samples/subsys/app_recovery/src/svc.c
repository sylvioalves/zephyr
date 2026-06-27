/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/app_platform/app_platform.h>

/*
 * Per-app checkpoint store, owned by the platform (supervisor side). It lives
 * outside any app's memory domain, so it survives an app crashing and being
 * restarted - which is exactly the point.
 */
static uint32_t checkpoints[CONFIG_APP_PLATFORM_MAX_APPS];

void z_impl_app_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

void z_impl_svc_checkpoint_set(uint32_t value)
{
	int id = app_platform_id_current();

	if (id >= 0) {
		checkpoints[id] = value;
	}
}

uint32_t z_impl_svc_checkpoint_get(void)
{
	int id = app_platform_id_current();

	return (id >= 0) ? checkpoints[id] : 0U;
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_app_log(const char *msg)
{
	char buf[64];

	if (k_usermode_string_copy(buf, (char *)msg, sizeof(buf)) == 0) {
		z_impl_app_log(buf);
	}
}
#include <zephyr/syscalls/app_log_mrsh.c>

static inline void z_vrfy_svc_checkpoint_set(uint32_t value)
{
	z_impl_svc_checkpoint_set(value);
}
#include <zephyr/syscalls/svc_checkpoint_set_mrsh.c>

static inline uint32_t z_vrfy_svc_checkpoint_get(void)
{
	return z_impl_svc_checkpoint_get();
}
#include <zephyr/syscalls/svc_checkpoint_get_mrsh.c>
#endif
