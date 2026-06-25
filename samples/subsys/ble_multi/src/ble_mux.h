/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_MULTI_BLE_MUX_H_
#define BLE_MULTI_BLE_MUX_H_

#include <zephyr/kernel.h>

/*
 * Multi-app BLE multiplexer - the daemon model at full width, like bluetoothd
 * or Android's Bluetooth service. The broker owns the single controller and
 * multiplexes many apps onto its pool of concurrent capabilities:
 *
 *   - ONE shared scanner, results fanned to every scanning app
 *   - up to BT_MAX_CONN concurrent connections; each app may hold its own
 *   - per-connection GATT subscriptions, notifications routed to the owning app
 *
 * App A scanning, App B connected to peripheral X, and App C connected to
 * peripheral Y all run at the same time. Each app sees only its own events,
 * delivered to its own queue. The radio is shared and arbitrated; the only
 * ceiling is the controller's pool size (BLE physics, same as a phone).
 */

#define BLE_MUX_MAX_APPS 4
#define BLE_ADDR_STRLEN  30
#define BLE_VAL_MAXLEN   20

enum ble_evt_kind {
	BLE_EVT_SCAN = 0,    /* a device was discovered             */
	BLE_EVT_CONNECTED,   /* this app's connection is up         */
	BLE_EVT_SUBSCRIBED,  /* this app's subscription is active   */
	BLE_EVT_NOTIFY,      /* a notification on this app's conn   */
	BLE_EVT_DISCONNECTED,
};

/* One event delivered to an app's queue. */
struct ble_event {
	uint8_t kind;
	char    addr[BLE_ADDR_STRLEN];
	int8_t  rssi;
	uint8_t len;
	uint8_t val[BLE_VAL_MAXLEN];
};

/* Initialize Bluetooth and the multiplexer. */
int ble_mux_init(void);

/* Register an app: its thread and its event queue. Returns app id or negative. */
int ble_mux_register(k_tid_t tid, struct k_msgq *q);

/* App requests: start sharing the scan / connect to a Heart Rate peer. */
int ble_mux_scan_start(int app_id);
int ble_mux_connect_hrs(int app_id);

#endif /* BLE_MULTI_BLE_MUX_H_ */
