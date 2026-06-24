/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_SERVICE_BLE_BROKER_H_
#define BLE_SERVICE_BLE_BROKER_H_

#include <zephyr/kernel.h>

/*
 * BLE scan service - the daemon model, like bluetoothd or Android's Bluetooth
 * service. The broker owns the single controller and runs ONE scan; every
 * discovered device is fanned out to each subscribed app. Apps never touch the
 * controller or HCI; they receive scan events asynchronously through a per-app
 * event queue, which is how BLE's unsolicited events cross into isolated apps.
 *
 * This is the async-event seam the higher BLE tiers (GATT client notifications,
 * server requests) build on.
 */

#define BLE_MAX_APPS    4
#define BLE_ADDR_STRLEN 30

/* One scan event delivered to an app. */
struct ble_scan_event {
	char    addr[BLE_ADDR_STRLEN];
	int8_t  rssi;
	uint8_t adv_type;
	uint8_t ad_len;
};

/* Initialize Bluetooth and the broker. Call once at startup. */
int ble_broker_init(void);

/*
 * Subscribe the calling context to scan events; returns a subscriber id (>=0)
 * or negative on error. Starts the shared scan on the first subscriber.
 */
int ble_broker_subscribe(struct k_msgq *out_q);

/* Stop scanning if no subscribers remain. */
void ble_broker_unsubscribe(int sub_id);

#endif /* BLE_SERVICE_BLE_BROKER_H_ */
