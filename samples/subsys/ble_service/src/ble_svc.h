/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_SERVICE_BLE_SVC_H_
#define BLE_SERVICE_BLE_SVC_H_

#include <zephyr/kernel.h>
#include "ble_broker.h"

/*
 * Broker syscalls a user-mode app uses to consume the BLE scan service. The app
 * subscribes once, then pulls scan events from its own queue. The events are
 * pushed by the broker (in supervisor context) as devices are discovered - the
 * async-event seam that brings BLE's unsolicited events into an isolated app.
 */

/* Supervisor-side: register an app thread and its granted event queue. */
void ble_svc_register(k_tid_t tid, struct k_msgq *q);

/* Subscribe the calling app to BLE scan events. Returns a sub id or negative. */
__syscall int svc_ble_scan_start(void);

/*
 * Wait up to @p timeout_ms for the next scan event into @p out. Returns 0 on an
 * event, -EAGAIN on timeout. Read-only consumption: many apps do this in
 * parallel, each draining its own queue.
 */
__syscall int svc_ble_next_device(struct ble_scan_event *out, int32_t timeout_ms);

#include <zephyr/syscalls/ble_svc.h>

#endif /* BLE_SERVICE_BLE_SVC_H_ */
