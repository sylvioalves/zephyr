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

void z_impl_app_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

void z_impl_app_beat(void)
{
	app_platform_heartbeat();
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

static inline void z_vrfy_app_beat(void)
{
	z_impl_app_beat();
}
#include <zephyr/syscalls/app_beat_mrsh.c>
#endif
