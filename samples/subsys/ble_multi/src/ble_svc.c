/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_svc.h"
#include "ble_mux.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>

struct app_reg {
	k_tid_t        tid;
	struct k_msgq *q;
	int            app_id;
};

static struct app_reg regs[BLE_MUX_MAX_APPS];

void ble_svc_register(k_tid_t tid, struct k_msgq *q)
{
	int id = ble_mux_register(tid, q);

	if (id < 0) {
		return;
	}
	for (int i = 0; i < BLE_MUX_MAX_APPS; i++) {
		if (regs[i].tid == NULL) {
			regs[i].tid = tid;
			regs[i].q = q;
			regs[i].app_id = id;
			return;
		}
	}
}

static struct app_reg *reg_of_current(void)
{
	k_tid_t cur = k_current_get();

	for (int i = 0; i < BLE_MUX_MAX_APPS; i++) {
		if (regs[i].tid == cur) {
			return &regs[i];
		}
	}
	return NULL;
}

int z_impl_svc_ble_scan(void)
{
	struct app_reg *r = reg_of_current();

	return r ? ble_mux_scan_start(r->app_id) : -EPERM;
}

int z_impl_svc_ble_connect(void)
{
	struct app_reg *r = reg_of_current();

	return r ? ble_mux_connect_hrs(r->app_id) : -EPERM;
}

int z_impl_svc_ble_next(struct ble_event *out, int32_t timeout_ms)
{
	struct app_reg *r = reg_of_current();

	if (!r || !r->q) {
		return -EPERM;
	}
	return (k_msgq_get(r->q, out, K_MSEC(timeout_ms)) == 0) ? 0 : -EAGAIN;
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_svc_ble_scan(void)
{
	return z_impl_svc_ble_scan();
}
#include <zephyr/syscalls/svc_ble_scan_mrsh.c>

static inline int z_vrfy_svc_ble_connect(void)
{
	return z_impl_svc_ble_connect();
}
#include <zephyr/syscalls/svc_ble_connect_mrsh.c>

static inline int z_vrfy_svc_ble_next(struct ble_event *out, int32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_ble_next(out, timeout_ms);
}
#include <zephyr/syscalls/svc_ble_next_mrsh.c>

#endif /* CONFIG_USERSPACE */
