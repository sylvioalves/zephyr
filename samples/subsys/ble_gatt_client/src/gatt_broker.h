/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_GATT_CLIENT_GATT_BROKER_H_
#define BLE_GATT_CLIENT_GATT_BROKER_H_

#include <zephyr/kernel.h>

/*
 * BLE GATT client service (Tier 2). The broker owns the controller and runs the
 * whole stateful client machine in supervisor mode: scan for a peer exposing the
 * Heart Rate Service, connect, discover the measurement characteristic, and
 * subscribe to its notifications. Each notification is pushed to the requesting
 * app's event queue - the same async-event seam used for scan results, now
 * carrying connection-bound data.
 *
 * The app never touches the controller, the connection, or the GATT layer; it
 * asks for "heart rate" and receives values asynchronously.
 */

#define GATT_NOTIFY_MAXLEN 20

/* One notification delivered to an app. */
struct gatt_notification {
	uint16_t handle;
	uint8_t  len;
	uint8_t  data[GATT_NOTIFY_MAXLEN];
};

/* Connection-state events the app can observe. */
enum gatt_state {
	GATT_SCANNING = 0,
	GATT_CONNECTED,
	GATT_SUBSCRIBED,
	GATT_DISCONNECTED,
};

/* Initialize Bluetooth and the GATT-client broker. */
int gatt_broker_init(void);

/*
 * Start the client machine: scan, connect to a Heart Rate peer, discover, and
 * subscribe. Notifications and state changes are delivered to @p notify_q.
 */
int gatt_broker_start(struct k_msgq *notify_q);

/* Current state (for the app to report). */
enum gatt_state gatt_broker_state(void);

#endif /* BLE_GATT_CLIENT_GATT_BROKER_H_ */
