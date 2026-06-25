/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_MULTI_BLE_SVC_H_
#define BLE_MULTI_BLE_SVC_H_

#include <zephyr/kernel.h>
#include "ble_mux.h"

/*
 * Per-app broker syscalls. Each isolated app asks the multiplexer to scan or to
 * connect, then drains its own event queue. Many apps call these concurrently;
 * the broker routes each app's events (scan results, its connection's
 * notifications) to its own queue.
 */

/* Supervisor-side: register an app's thread and event queue. */
void ble_svc_register(k_tid_t tid, struct k_msgq *q);

/* Join the shared scan. */
__syscall int svc_ble_scan(void);

/* Connect to a Heart Rate peer for this app. */
__syscall int svc_ble_connect(void);

/* Wait up to timeout_ms for the next event into @p out. 0 on event, -EAGAIN. */
__syscall int svc_ble_next(struct ble_event *out, int32_t timeout_ms);

#include <zephyr/syscalls/ble_svc.h>

#endif /* BLE_MULTI_BLE_SVC_H_ */
