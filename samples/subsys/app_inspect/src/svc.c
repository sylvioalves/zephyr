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

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

int z_impl_svc_app_count(void)
{
	return app_platform_count();
}

int z_impl_svc_app_info(int id, struct svc_app_info *out)
{
	if ((app_platform_caps_current() & CAP_INSPECT) == 0U) {
		return -EPERM;
	}

	struct app_platform_status st;
	int ret = app_platform_status(id, &st);

	if (ret != 0) {
		return ret;
	}

	*out = (struct svc_app_info){
		.active = st.active,
		.monitored = st.monitored,
		.faulted = st.faulted,
		.stack_size = st.stack_size,
		.stack_used = st.stack_used,
		.cpu_cycles = st.cpu_cycles,
		.cpu_total = st.cpu_total,
	};
	strncpy(out->name, st.name ? st.name : "", sizeof(out->name) - 1);

	return 0;
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_svc_log(const char *msg)
{
	char buf[96];

	if (k_usermode_string_copy(buf, (char *)msg, sizeof(buf)) == 0) {
		z_impl_svc_log(buf);
	}
}
#include <zephyr/syscalls/svc_log_mrsh.c>

static inline int z_vrfy_svc_app_count(void)
{
	return z_impl_svc_app_count();
}
#include <zephyr/syscalls/svc_app_count_mrsh.c>

static inline int z_vrfy_svc_app_info(int id, struct svc_app_info *out)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_app_info(id, out);
}
#include <zephyr/syscalls/svc_app_info_mrsh.c>
#endif
