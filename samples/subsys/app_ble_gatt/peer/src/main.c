/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The peer for the generic GATT sample. It advertises as "gatt-peer" and
 * exposes two services so the device can show that a grant bounds what an
 * application may reach:
 *
 *   sensor service   a1b2c3e0-...  one readable and one writable value
 *   private service  a1b2c3e1-...  a readable value the device is not granted
 *
 * The device application is granted only the sensor service, so its read of
 * the private service is refused by its own platform before any GATT traffic
 * is generated.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <string.h>

static const struct bt_uuid_128 sensor_svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3e0, 0x2222, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 sensor_val_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3e1, 0x2222, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 sensor_cfg_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3e2, 0x2222, 0x4a5b, 0x8c7d, 0x0123456789ab));

static const struct bt_uuid_128 private_svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3f0, 0x3333, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 private_val_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3f1, 0x3333, 0x4a5b, 0x8c7d, 0x0123456789ab));

static uint8_t sensor_value[4] = { 0x21, 0x43, 0x65, 0x87 };
static uint8_t sensor_cfg[4];
static uint8_t private_value[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

static ssize_t val_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
				 sizeof(sensor_value));
}

static ssize_t cfg_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);

	if (offset + len > sizeof(sensor_cfg)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(&sensor_cfg[offset], buf, len);
	printk("peer: config written, %u byte(s), first 0x%02x\n", len,
	       sensor_cfg[0]);
	return len;
}

BT_GATT_SERVICE_DEFINE(sensor_svc,
	BT_GATT_PRIMARY_SERVICE(&sensor_svc_uuid),
	BT_GATT_CHARACTERISTIC(&sensor_val_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, val_read, NULL, sensor_value),
	BT_GATT_CHARACTERISTIC(&sensor_cfg_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, cfg_write, sensor_cfg),
);

BT_GATT_SERVICE_DEFINE(private_svc,
	BT_GATT_PRIMARY_SERVICE(&private_svc_uuid),
	BT_GATT_CHARACTERISTIC(&private_val_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, val_read, NULL,
			       private_value),
);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Each service call is its own connection, so the peer has to advertise again
 * once the device drops the link; a peripheral does not resume on its own.
 * Restart once the connection object is free, or a connectable advertiser has
 * no connection to accept with. The previous advertising set is still
 * registered, so stop it first.
 */
static void peer_recycled(void)
{
	(void)bt_le_adv_stop();

	int rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);

	if (rc != 0) {
		printk("peer: advertising again failed (%d)\n", rc);
	}
}

BT_CONN_CB_DEFINE(peer_conn_cb) = {
	.recycled = peer_recycled,
};

int main(void)
{
	printk("\n*** BLE GATT peer: advertising as '%s' ***\n",
	       CONFIG_BT_DEVICE_NAME);

	int rc = bt_enable(NULL);

	if (rc != 0) {
		printk("bt_enable failed (%d)\n", rc);
		return 0;
	}

	rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
	if (rc != 0) {
		printk("advertising failed (%d)\n", rc);
		return 0;
	}
	printk("peer: sensor and private services registered\n");
	return 0;
}
