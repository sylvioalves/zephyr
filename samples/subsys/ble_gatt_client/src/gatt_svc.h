/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_GATT_CLIENT_GATT_SVC_H_
#define BLE_GATT_CLIENT_GATT_SVC_H_

#include <zephyr/kernel.h>
#include "gatt_broker.h"

/*
 * Broker syscalls for the GATT-client service. The app asks the broker to find,
 * connect to, and subscribe to a Heart Rate peer, then receives notifications
 * and connection-state changes asynchronously - the same per-app event queue
 * mechanism as the scan service, now carrying connection-bound data.
 */

/* Supervisor-side: register the calling app's thread and event queue. */
void gatt_svc_register(k_tid_t tid, struct k_msgq *q);

/* Start the client machine (scan -> connect -> subscribe). */
__syscall int svc_ble_hrs_start(void);

/*
 * Wait up to @p timeout_ms for the next event (notification or state change).
 * Returns 0 on an event, -EAGAIN on timeout.
 */
__syscall int svc_ble_next_event(struct gatt_notification *out,
				 int32_t timeout_ms);

#include <zephyr/syscalls/gatt_svc.h>

#endif /* BLE_GATT_CLIENT_GATT_SVC_H_ */
