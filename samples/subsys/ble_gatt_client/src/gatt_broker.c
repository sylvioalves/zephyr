/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gatt_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/addr.h>

/* The app's notification/state queue (set by gatt_broker_start). */
static struct k_msgq *app_q;

static struct bt_conn *default_conn;
static struct bt_uuid_16 discover_uuid = BT_UUID_INIT_16(0);
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_discover_params ccc_disc_params;   /* for auto-CCC */
static struct bt_gatt_subscribe_params subscribe_params;
static enum gatt_state state;

static void push_state(enum gatt_state s)
{
	state = s;
	/* A zero-length notification carries a state change to the app. */
	struct gatt_notification ev = { .handle = 0xFFFF, .len = 0 };

	ev.data[0] = (uint8_t)s;
	if (app_q) {
		(void)k_msgq_put(app_q, &ev, K_NO_WAIT);
	}
}

static void start_scan(void);

/* A heart-rate notification arrived: push the raw value to the app. */
static uint8_t notify_func(struct bt_conn *conn,
			   struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	if (!data) {
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	struct gatt_notification ev = {
		.handle = params->value_handle,
		.len = MIN(length, GATT_NOTIFY_MAXLEN),
	};

	memcpy(ev.data, data, ev.len);
	if (app_q) {
		(void)k_msgq_put(app_q, &ev, K_NO_WAIT);
	}
	return BT_GATT_ITER_CONTINUE;
}

/* GATT discovery state machine: primary -> characteristic -> CCC -> subscribe. */
static uint8_t discover_func(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err;

	if (!attr) {
		return BT_GATT_ITER_STOP;
	}

	if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_HRS)) {
		memcpy(&discover_uuid, BT_UUID_HRS_MEASUREMENT,
		       sizeof(discover_uuid));
		discover_params.uuid = &discover_uuid.uuid;
		discover_params.start_handle = attr->handle + 1;
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		discover_params.func = discover_func;   /* keep callback valid */
		err = bt_gatt_discover(conn, &discover_params);
		if (err) {
			printk("gatt-broker: char discover failed (%d)\n", err);
		}
	} else {
		/*
		 * Found the measurement characteristic. Let the subscribe call
		 * auto-discover the CCC descriptor (BT_GATT_AUTO_DISCOVER_CCC),
		 * avoiding a separate manual descriptor-discovery pass.
		 */
		subscribe_params.notify = notify_func;
		subscribe_params.value = BT_GATT_CCC_NOTIFY;
		subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);
		subscribe_params.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
		subscribe_params.disc_params = &ccc_disc_params;
		subscribe_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;

		err = bt_gatt_subscribe(conn, &subscribe_params);
		if (err && err != -EALREADY) {
			printk("gatt-broker: subscribe failed (%d)\n", err);
		} else {
			printk("gatt-broker: subscribed to heart rate\n");
			push_state(GATT_SUBSCRIBED);
		}
		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_CONTINUE;
}

/* Advertised-data parser: connect if the peer offers the Heart Rate Service. */
static bool eir_found(struct bt_data *data, void *user_data)
{
	bt_addr_le_t *addr = user_data;

	if (data->type != BT_DATA_UUID16_SOME &&
	    data->type != BT_DATA_UUID16_ALL) {
		return true;
	}

	for (int i = 0; i + 1 < data->data_len; i += 2) {
		struct bt_uuid_16 u = BT_UUID_INIT_16(
			sys_get_le16(&data->data[i]));

		if (bt_uuid_cmp(&u.uuid, BT_UUID_HRS) != 0) {
			continue;
		}

		(void)bt_le_scan_stop();

		struct bt_conn_le_create_param *cp = BT_CONN_LE_CREATE_CONN;

		int err = bt_conn_le_create(addr, cp, BT_LE_CONN_PARAM_DEFAULT,
					    &default_conn);
		if (err) {
			printk("gatt-broker: create conn failed (%d)\n", err);
			start_scan();
		}
		return false;
	}
	return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	if (default_conn) {
		return;
	}
	if (type == BT_GAP_ADV_TYPE_ADV_IND ||
	    type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		bt_data_parse(ad, eir_found, (void *)addr);
	}
}

static void start_scan(void)
{
	struct bt_le_scan_param sp = {
		.type    = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window  = BT_GAP_SCAN_FAST_WINDOW,
	};

	int err = bt_le_scan_start(&sp, device_found);

	if (err) {
		printk("gatt-broker: scan start failed (%d)\n", err);
		return;
	}
	push_state(GATT_SCANNING);
	printk("gatt-broker: scanning for a Heart Rate peer\n");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("gatt-broker: connect failed (%u)\n", err);
		bt_conn_unref(default_conn);
		default_conn = NULL;
		start_scan();
		return;
	}

	printk("gatt-broker: %s, discovering services\n", __func__);
	push_state(GATT_CONNECTED);

	memcpy(&discover_uuid, BT_UUID_HRS, sizeof(discover_uuid));
	discover_params.uuid = &discover_uuid.uuid;
	discover_params.func = discover_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_PRIMARY;

	if (bt_gatt_discover(conn, &discover_params) != 0) {
		printk("gatt-broker: discover failed\n");
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("gatt-broker: %s (0x%02x), rescanning\n", __func__, reason);
	bt_conn_unref(default_conn);
	default_conn = NULL;
	push_state(GATT_DISCONNECTED);
	start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

int gatt_broker_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		printk("gatt-broker: bt_enable failed (%d)\n", err);
		return err;
	}
	printk("gatt-broker: Bluetooth ready, broker owns the controller\n");
	return 0;
}

int gatt_broker_start(struct k_msgq *notify_q)
{
	app_q = notify_q;
	start_scan();
	return 0;
}

enum gatt_state gatt_broker_state(void)
{
	return state;
}
