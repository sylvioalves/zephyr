/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"
#include "app_caps.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/app_platform/app_platform.h>
#include <string.h>
#include <errno.h>

/* The catalog of workers the manager app may start, resolved by name. */
extern const struct app_def *control_catalog_find(const char *name);

void z_impl_app_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

void z_impl_app_beat(void)
{
	app_platform_heartbeat();
}

int z_impl_svc_ctrl_start(const char *name)
{
	if ((app_platform_caps_current() & CAP_CONTROL) == 0U) {
		return -EPERM;
	}

	const struct app_def *def = control_catalog_find(name);

	if (def == NULL) {
		return -ENOENT;
	}
	return app_platform_launch(def);
}

int z_impl_svc_ctrl_stop(int id)
{
	if ((app_platform_caps_current() & CAP_CONTROL) == 0U) {
		return -EPERM;
	}
	return app_platform_stop(id);
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

static inline int z_vrfy_svc_ctrl_start(const char *name)
{
	char buf[16];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_ctrl_start(buf);
}
#include <zephyr/syscalls/svc_ctrl_start_mrsh.c>

static inline int z_vrfy_svc_ctrl_stop(int id)
{
	return z_impl_svc_ctrl_stop(id);
}
#include <zephyr/syscalls/svc_ctrl_stop_mrsh.c>
#endif
