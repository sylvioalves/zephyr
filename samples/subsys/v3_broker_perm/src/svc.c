/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>

static uint32_t service_calls;

int z_impl_svc_request(struct k_sem *token)
{
	ARG_UNUSED(token);
	/* The service action: trivial, runs in supervisor mode. */
	service_calls++;
	return (int)service_calls;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_request(struct k_sem *token)
{
	/*
	 * Per-app service gate: the caller must hold permission on the service
	 * token kobject. An ungranted app fails here (K_OOPS -> only the
	 * faulting app is killed); a granted app passes through.
	 */
	K_OOPS(K_SYSCALL_OBJ(token, K_OBJ_SEM));
	return z_impl_svc_request(token);
}
#include <zephyr/syscalls/svc_request_mrsh.c>
#endif /* CONFIG_USERSPACE */
