/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The central for the peripheral sample: it stands in for a phone. It scans
 * for the advertised name, discovers the served characteristics, reads the
 * counter and writes the config value, which the application on the other
 * board receives as an input event.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>
#include <string.h>

#define PEER_NAME "c5-app"

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3a0, 0x4444, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 count_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3a1, 0x4444, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 conf_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3a2, 0x4444, 0x4a5b, 0x8c7d, 0x0123456789ab));

static struct bt_conn *conn;
static struct bt_uuid_128 disc_uuid = BT_UUID_INIT_128(0);
static struct bt_gatt_discover_params disc;
static struct bt_gatt_read_params rd;
static struct bt_gatt_write_params wr;
static uint16_t count_handle;
static uint16_t conf_handle;
static bool connect_wanted = true;

static K_SEM_DEFINE(ready, 0, 1);
static K_SEM_DEFINE(step, 0, 1);

static uint8_t discover_cb(struct bt_conn *c, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	if (attr == NULL) {
		k_sem_give(&ready);
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		memcpy(&disc_uuid, &count_uuid, sizeof(disc_uuid));
		disc.uuid = &disc_uuid.uuid;
		disc.start_handle = attr->handle + 1;
		disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		bt_gatt_discover(c, &disc);
	} else if (count_handle == 0U) {
		count_handle = bt_gatt_attr_value_handle(attr);
		memcpy(&disc_uuid, &conf_uuid, sizeof(disc_uuid));
		disc.uuid = &disc_uuid.uuid;
		disc.start_handle = attr->handle + 2;
		disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		bt_gatt_discover(c, &disc);
	} else {
		conf_handle = bt_gatt_attr_value_handle(attr);
		k_sem_give(&ready);
	}
	return BT_GATT_ITER_STOP;
}

static uint8_t read_cb(struct bt_conn *c, uint8_t err,
		       struct bt_gatt_read_params *params, const void *data,
		       uint16_t len)
{
	ARG_UNUSED(c); ARG_UNUSED(params);

	if (err == 0U && data != NULL && len > 0U) {
		const uint8_t *b = data;

		printk("central: read %u byte(s), first 0x%02x\n", len, b[0]);
	} else if (data == NULL) {
		k_sem_give(&step);
		return BT_GATT_ITER_STOP;
	}
	return BT_GATT_ITER_CONTINUE;
}

static void write_cb(struct bt_conn *c, uint8_t err,
		     struct bt_gatt_write_params *params)
{
	ARG_UNUSED(c); ARG_UNUSED(params);
	printk("central: write done err=%u\n", err);
	k_sem_give(&step);
}

static void connected(struct bt_conn *c, uint8_t err)
{
	if (err != 0U) {
		printk("central: connect failed 0x%02x\n", err);
		return;
	}
	printk("central: %s\n", __func__);
	memcpy(&disc_uuid, &svc_uuid, sizeof(disc_uuid));
	disc.uuid = &disc_uuid.uuid;
	disc.func = discover_cb;
	disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc.type = BT_GATT_DISCOVER_PRIMARY;
	bt_gatt_discover(c, &disc);
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
	printk("central: %s 0x%02x\n", __func__, reason);
	if (c == conn) {
		bt_conn_unref(conn);
		conn = NULL;
	}
}

BT_CONN_CB_DEFINE(central_cb) = {
	.connected = connected,
	.disconnected = disconnected,
};

static bool name_cb(struct bt_data *data, void *user)
{
	bool *found = user;

	if (data->type == BT_DATA_NAME_COMPLETE ||
	    data->type == BT_DATA_NAME_SHORTENED) {
		if (data->data_len == strlen(PEER_NAME) &&
		    memcmp(data->data, PEER_NAME, data->data_len) == 0) {
			*found = true;
			return false;
		}
	}
	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		    struct net_buf_simple *ad)
{
	ARG_UNUSED(rssi); ARG_UNUSED(type);

	if (!connect_wanted || conn != NULL) {
		return;
	}

	bool found = false;

	bt_data_parse(ad, name_cb, &found);
	if (!found) {
		return;
	}
	bt_le_scan_stop();
	printk("central: found '%s', connecting\n", PEER_NAME);
	(void)bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT, &conn);
}

int main(void)
{
	printk("\n*** BLE central: driving the served application ***\n");

	if (bt_enable(NULL) != 0) {
		printk("central: bt_enable failed\n");
		return 0;
	}

	struct bt_le_scan_param sp = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	if (bt_le_scan_start(&sp, scan_cb) != 0) {
		printk("central: scan failed\n");
		return 0;
	}
	if (k_sem_take(&ready, K_SECONDS(20)) != 0) {
		printk("central: peer not found or discovery failed\n");
		return 0;
	}
	if (count_handle == 0U || conf_handle == 0U) {
		printk("central: characteristics not found\n");
		return 0;
	}
	printk("central: discovered count=%u conf=%u\n", count_handle,
	       conf_handle);

	for (int i = 0; i < 3; i++) {
		rd.func = read_cb;
		rd.handle_count = 1;
		rd.single.handle = count_handle;
		rd.single.offset = 0;
		k_sem_reset(&step);
		if (bt_gatt_read(conn, &rd) == 0) {
			(void)k_sem_take(&step, K_SECONDS(5));
		}

		uint8_t v[2] = { (uint8_t)(0xA0 + i), 0x5A };

		wr.handle = conf_handle;
		wr.offset = 0;
		wr.data = v;
		wr.length = sizeof(v);
		wr.func = write_cb;
		k_sem_reset(&step);
		if (bt_gatt_write(conn, &wr) == 0) {
			(void)k_sem_take(&step, K_SECONDS(5));
		}
		k_sleep(K_SECONDS(2));
	}

	printk("central: done\n");
	connect_wanted = false;
	if (conn != NULL) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	return 0;
}
