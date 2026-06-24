/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>

/* Per-app subscriber: an event queue the app drains. */
static struct k_msgq *subscribers[BLE_MAX_APPS];
static int subscriber_count;
static bool scanning;
static struct k_mutex lock;

/*
 * The single scan callback. One radio scans once; each discovered device is
 * copied into every subscriber's queue (drop if full - apps that fall behind
 * just miss events, they are never blocked on). This is the fan-out that lets
 * many isolated apps "all scan" through one controller.
 */
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	struct ble_scan_event ev = {
		.rssi = rssi,
		.adv_type = type,
		.ad_len = ad ? ad->len : 0,
	};

	bt_addr_le_to_str(addr, ev.addr, sizeof(ev.addr));

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < BLE_MAX_APPS; i++) {
		if (subscribers[i] != NULL) {
			(void)k_msgq_put(subscribers[i], &ev, K_NO_WAIT);
		}
	}
	k_mutex_unlock(&lock);
}

static int scan_start(void)
{
	struct bt_le_scan_param param = {
		.type    = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL_MIN,
		.window  = BT_GAP_SCAN_FAST_WINDOW,
	};

	return bt_le_scan_start(&param, device_found);
}

int ble_broker_init(void)
{
	k_mutex_init(&lock);

	int err = bt_enable(NULL);

	if (err) {
		printk("ble-broker: bt_enable failed: %d\n", err);
		return err;
	}
	printk("ble-broker: Bluetooth ready, broker owns the controller\n");
	return 0;
}

int ble_broker_subscribe(struct k_msgq *out_q)
{
	int id = -ENOSPC;

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < BLE_MAX_APPS; i++) {
		if (subscribers[i] == NULL) {
			subscribers[i] = out_q;
			subscriber_count++;
			id = i;
			break;
		}
	}

	if (id >= 0 && !scanning) {
		if (scan_start() == 0) {
			scanning = true;
			printk("ble-broker: shared scan started\n");
		}
	}
	k_mutex_unlock(&lock);
	return id;
}

void ble_broker_unsubscribe(int sub_id)
{
	if (sub_id < 0 || sub_id >= BLE_MAX_APPS) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (subscribers[sub_id] != NULL) {
		subscribers[sub_id] = NULL;
		subscriber_count--;
	}
	if (subscriber_count == 0 && scanning) {
		bt_le_scan_stop();
		scanning = false;
	}
	k_mutex_unlock(&lock);
}
