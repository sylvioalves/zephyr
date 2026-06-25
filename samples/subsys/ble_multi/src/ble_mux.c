/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_mux.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/addr.h>

/* ---- per-app registration ------------------------------------------------ */

struct app_slot {
	k_tid_t        tid;
	struct k_msgq *q;
	bool           scanning;     /* subscribed to the shared scan        */
	bool           wants_conn;   /* asked to connect to a peer           */
};

static struct app_slot apps[BLE_MUX_MAX_APPS];
static int app_count;
static struct k_mutex lock;
static bool scan_running;

/* ---- per-connection table (keyed by bt_conn index) ----------------------- */

struct conn_ctx {
	struct bt_conn                 *conn;
	int                             owner;   /* app id that owns this link */
	struct bt_gatt_discover_params  disc;
	struct bt_gatt_discover_params  ccc_disc;
	struct bt_gatt_subscribe_params sub;
	struct bt_uuid_16               uuid;
};

static struct conn_ctx conns[CONFIG_BT_MAX_CONN];

static struct conn_ctx *ctx_by_conn(struct bt_conn *conn)
{
	uint8_t i = bt_conn_index(conn);

	return (i < CONFIG_BT_MAX_CONN) ? &conns[i] : NULL;
}

/* Deliver an event to a specific app's queue. */
static void deliver(int app_id, const struct ble_event *ev)
{
	if (app_id >= 0 && app_id < BLE_MUX_MAX_APPS && apps[app_id].q) {
		(void)k_msgq_put(apps[app_id].q, ev, K_NO_WAIT);
	}
}

/* ---- shared scan: fan results to every scanning app ---------------------- */

static int pending_connector = -1;   /* app id awaiting a connection        */

static void try_connect_hrs(const bt_addr_le_t *addr);
static int shared_scan_start(void);

/* Advert parser: true to keep parsing; connects if the peer offers HRS. */
static bool adv_has_hrs(struct bt_data *data, void *user_data)
{
	const bt_addr_le_t *addr = user_data;

	if (data->type != BT_DATA_UUID16_SOME &&
	    data->type != BT_DATA_UUID16_ALL) {
		return true;
	}
	for (int i = 0; i + 1 < data->data_len; i += 2) {
		struct bt_uuid_16 u = BT_UUID_INIT_16(
			sys_get_le16(&data->data[i]));

		if (bt_uuid_cmp(&u.uuid, BT_UUID_HRS) == 0) {
			try_connect_hrs(addr);
			return false;
		}
	}
	return true;
}

static void scan_recv(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		      struct net_buf_simple *ad)
{
	struct ble_event ev = { .kind = BLE_EVT_SCAN, .rssi = rssi };

	bt_addr_le_to_str(addr, ev.addr, sizeof(ev.addr));

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < BLE_MUX_MAX_APPS; i++) {
		if (apps[i].scanning) {
			deliver(i, &ev);
		}
	}
	k_mutex_unlock(&lock);

	/* If an app is waiting to connect, look for an HR peer in this advert.
	 * bt_data_parse consumes the buffer, so use a clone for the scan event
	 * fan-out above (already done) and parse here.
	 */
	if (pending_connector >= 0 &&
	    (type == BT_GAP_ADV_TYPE_ADV_IND ||
	     type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND)) {
		bt_data_parse(ad, adv_has_hrs, (void *)addr);
	}
}

static int shared_scan_start(void)
{
	if (scan_running) {
		return 0;
	}
	struct bt_le_scan_param sp = {
		.type    = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window  = BT_GAP_SCAN_FAST_WINDOW,
	};
	int err = bt_le_scan_start(&sp, scan_recv);

	if (err == 0) {
		scan_running = true;
		printk("ble-mux: shared scan running\n");
	}
	return err;
}

/* ---- GATT client machine (per connection) -------------------------------- */

static uint8_t notify_cb(struct bt_conn *conn,
			 struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	struct conn_ctx *c = ctx_by_conn(conn);

	if (!data || !c) {
		if (params) {
			params->value_handle = 0U;
		}
		return BT_GATT_ITER_STOP;
	}

	struct ble_event ev = { .kind = BLE_EVT_NOTIFY,
				.len = MIN(length, BLE_VAL_MAXLEN) };

	memcpy(ev.val, data, ev.len);
	bt_addr_le_to_str(bt_conn_get_dst(conn), ev.addr, sizeof(ev.addr));
	deliver(c->owner, &ev);
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	struct conn_ctx *c = ctx_by_conn(conn);

	if (!attr || !c) {
		return BT_GATT_ITER_STOP;
	}

	if (!bt_uuid_cmp(c->disc.uuid, BT_UUID_HRS)) {
		memcpy(&c->uuid, BT_UUID_HRS_MEASUREMENT, sizeof(c->uuid));
		c->disc.uuid = &c->uuid.uuid;
		c->disc.start_handle = attr->handle + 1;
		c->disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		(void)bt_gatt_discover(conn, &c->disc);
	} else {
		c->sub.notify = notify_cb;
		c->sub.value = BT_GATT_CCC_NOTIFY;
		c->sub.value_handle = bt_gatt_attr_value_handle(attr);
		c->sub.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
		c->sub.disc_params = &c->ccc_disc;
		c->sub.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

		int err = bt_gatt_subscribe(conn, &c->sub);

		if (!err || err == -EALREADY) {
			struct ble_event ev = { .kind = BLE_EVT_SUBSCRIBED };

			bt_addr_le_to_str(bt_conn_get_dst(conn), ev.addr,
					  sizeof(ev.addr));
			deliver(c->owner, &ev);
		}
		return BT_GATT_ITER_STOP;
	}
	return BT_GATT_ITER_CONTINUE;
}

static void try_connect_hrs(const bt_addr_le_t *addr)
{
	struct bt_conn *conn = NULL;
	struct bt_conn_le_create_param *cp = BT_CONN_LE_CREATE_CONN;
	int connector = pending_connector;

	/* Creating a connection requires the scanner to be stopped; restart it
	 * afterwards so scanning apps keep receiving results while the link is
	 * up (the controller interleaves scan windows with connection events).
	 */
	(void)bt_le_scan_stop();
	scan_running = false;

	int err = bt_conn_le_create(addr, cp, BT_LE_CONN_PARAM_DEFAULT, &conn);

	if (err == 0) {
		struct conn_ctx *c = ctx_by_conn(conn);

		if (c) {
			c->owner = connector;
		}
		pending_connector = -1;   /* claimed */
		bt_conn_unref(conn);
	} else {
		printk("ble-mux: connect create failed (%d)\n", err);
	}

	/* Resume the shared scan for the scanning apps. */
	(void)shared_scan_start();
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct conn_ctx *c = ctx_by_conn(conn);

	if (err || !c) {
		return;
	}
	c->conn = conn;

	struct ble_event ev = { .kind = BLE_EVT_CONNECTED };

	bt_addr_le_to_str(bt_conn_get_dst(conn), ev.addr, sizeof(ev.addr));
	deliver(c->owner, &ev);

	/* Begin service discovery for the heart-rate service. */
	memcpy(&c->uuid, BT_UUID_HRS, sizeof(c->uuid));
	c->disc.uuid = &c->uuid.uuid;
	c->disc.func = discover_cb;
	c->disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	c->disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	c->disc.type = BT_GATT_DISCOVER_PRIMARY;
	(void)bt_gatt_discover(conn, &c->disc);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct conn_ctx *c = ctx_by_conn(conn);

	if (!c) {
		return;
	}
	struct ble_event ev = { .kind = BLE_EVT_DISCONNECTED };

	bt_addr_le_to_str(bt_conn_get_dst(conn), ev.addr, sizeof(ev.addr));
	deliver(c->owner, &ev);
	c->conn = NULL;
	c->owner = -1;
}

BT_CONN_CB_DEFINE(mux_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* ---- public API ---------------------------------------------------------- */

int ble_mux_init(void)
{
	k_mutex_init(&lock);
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++) {
		conns[i].owner = -1;
	}

	int err = bt_enable(NULL);

	if (err) {
		printk("ble-mux: bt_enable failed (%d)\n", err);
		return err;
	}
	printk("ble-mux: Bluetooth ready, broker owns the controller\n");
	return 0;
}

int ble_mux_register(k_tid_t tid, struct k_msgq *q)
{
	for (int i = 0; i < BLE_MUX_MAX_APPS; i++) {
		if (apps[i].tid == NULL) {
			apps[i].tid = tid;
			apps[i].q = q;
			app_count++;
			return i;
		}
	}
	return -ENOSPC;
}

int ble_mux_scan_start(int app_id)
{
	if (app_id < 0 || app_id >= BLE_MUX_MAX_APPS) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	apps[app_id].scanning = true;
	int err = shared_scan_start();

	k_mutex_unlock(&lock);
	return err;
}

int ble_mux_connect_hrs(int app_id)
{
	if (app_id < 0 || app_id >= BLE_MUX_MAX_APPS) {
		return -EINVAL;
	}
	/* Mark this app as the pending connector and ensure the scan runs so we
	 * can find an HR peer. The controller scans while other links are up.
	 */
	k_mutex_lock(&lock, K_FOREVER);
	pending_connector = app_id;
	int err = shared_scan_start();

	k_mutex_unlock(&lock);
	return err;
}
