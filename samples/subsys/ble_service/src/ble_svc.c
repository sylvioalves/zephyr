/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_svc.h"
#include "ble_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>

/*
 * Per-app event-queue registry. The supervisor registers each app's thread and
 * its granted message queue before starting it; the broker syscalls resolve the
 * calling thread back to its queue, so the app never passes a kernel pointer.
 */
struct ble_app_reg {
	k_tid_t        tid;
	struct k_msgq *q;
	int            sub_id;
};

static struct ble_app_reg regs[BLE_MAX_APPS];

void ble_svc_register(k_tid_t tid, struct k_msgq *q)
{
	for (int i = 0; i < BLE_MAX_APPS; i++) {
		if (regs[i].tid == NULL) {
			regs[i].tid = tid;
			regs[i].q = q;
			regs[i].sub_id = -1;
			return;
		}
	}
}

static struct ble_app_reg *reg_of_current(void)
{
	k_tid_t cur = k_current_get();

	for (int i = 0; i < BLE_MAX_APPS; i++) {
		if (regs[i].tid == cur) {
			return &regs[i];
		}
	}
	return NULL;
}

int z_impl_svc_ble_scan_start(void)
{
	struct ble_app_reg *r = reg_of_current();

	if (r == NULL) {
		return -EPERM;
	}
	if (r->sub_id < 0) {
		r->sub_id = ble_broker_subscribe(r->q);
	}
	return r->sub_id;
}

int z_impl_svc_ble_next_device(struct ble_scan_event *out, int32_t timeout_ms)
{
	struct ble_app_reg *r = reg_of_current();

	if (r == NULL || r->q == NULL) {
		return -EPERM;
	}
	if (k_msgq_get(r->q, out, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
	}
	return 0;
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_svc_ble_scan_start(void)
{
	return z_impl_svc_ble_scan_start();
}
#include <zephyr/syscalls/svc_ble_scan_start_mrsh.c>

static inline int z_vrfy_svc_ble_next_device(struct ble_scan_event *out,
					     int32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_ble_next_device(out, timeout_ms);
}
#include <zephyr/syscalls/svc_ble_next_device_mrsh.c>

#endif /* CONFIG_USERSPACE */
