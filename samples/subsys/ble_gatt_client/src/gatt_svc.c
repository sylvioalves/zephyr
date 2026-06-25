/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gatt_svc.h"
#include "gatt_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>

static k_tid_t app_tid;
static struct k_msgq *app_q;

void gatt_svc_register(k_tid_t tid, struct k_msgq *q)
{
	app_tid = tid;
	app_q = q;
}

int z_impl_svc_ble_hrs_start(void)
{
	if (k_current_get() != app_tid) {
		return -EPERM;
	}
	return gatt_broker_start(app_q);
}

int z_impl_svc_ble_next_event(struct gatt_notification *out, int32_t timeout_ms)
{
	if (k_current_get() != app_tid || app_q == NULL) {
		return -EPERM;
	}
	if (k_msgq_get(app_q, out, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
	}
	return 0;
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_svc_ble_hrs_start(void)
{
	return z_impl_svc_ble_hrs_start();
}
#include <zephyr/syscalls/svc_ble_hrs_start_mrsh.c>

static inline int z_vrfy_svc_ble_next_event(struct gatt_notification *out,
					    int32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_ble_next_event(out, timeout_ms);
}
#include <zephyr/syscalls/svc_ble_next_event_mrsh.c>

#endif /* CONFIG_USERSPACE */
