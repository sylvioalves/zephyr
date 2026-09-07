/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The BLE side of an application update: a peer board that hosts one signed
 * application and serves it to a device that has no network connection.
 *
 * It advertises under the application's name and exposes two characteristics.
 * The device writes a 4-byte little-endian offset to the control point, then
 * reads the data characteristic to get the bytes at that offset. Served image
 * is the manifest, its signature, then the module:
 *
 *   [ manifest (76 B) ][ signature (64 B) ][ module ... ]
 *
 * This peer signs nothing at runtime: the manifest and signature were produced
 * on the build host and are compiled in, so the private key never reaches
 * either board.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <string.h>

/* The served image, laid out exactly as the device reads it. */
static const uint8_t served_manifest[] = {
#include "ota_app.manifest.inc"
};
static const uint8_t served_sig[] = {
#include "ota_app.sig.inc"
};
static const uint8_t served_module[] = {
#include "ota_app.inc"
};

#define HDR_LEN     (sizeof(served_manifest) + sizeof(served_sig))
#define IMAGE_LEN   (HDR_LEN + sizeof(served_module))
#define CHUNK_MAX   244U

static uint8_t image_byte(size_t off)
{
	if (off < sizeof(served_manifest)) {
		return served_manifest[off];
	}
	if (off < HDR_LEN) {
		return served_sig[off - sizeof(served_manifest)];
	}
	return served_module[off - HDR_LEN];
}

static uint32_t read_offset;

static const struct bt_uuid_128 ota_svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3d0, 0x1111, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 ota_ctrl_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3d1, 0x1111, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 ota_data_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3d2, 0x1111, 0x4a5b, 0x8c7d, 0x0123456789ab));

static ssize_t ctrl_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset,
			  uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);

	if (len != sizeof(uint32_t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	read_offset = sys_get_le32(buf);
	return len;
}

static ssize_t data_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(attr);

	if (read_offset >= IMAGE_LEN) {
		return 0;
	}

	static uint8_t chunk[CHUNK_MAX];
	size_t avail = MIN((size_t)(IMAGE_LEN - read_offset), sizeof(chunk));

	for (size_t i = 0; i < avail; i++) {
		chunk[i] = image_byte(read_offset + i);
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, chunk, avail);
}

BT_GATT_SERVICE_DEFINE(ota_svc,
	BT_GATT_PRIMARY_SERVICE(&ota_svc_uuid),
	BT_GATT_CHARACTERISTIC(&ota_ctrl_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, ctrl_write, NULL),
	BT_GATT_CHARACTERISTIC(&ota_data_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, data_read, NULL, NULL),
);

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

int main(void)
{
	printk("\n*** BLE app-update peer: serving '%s' ***\n",
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
	printk("advertising; image is %zu bytes (%zu header + %zu module)\n",
	       (size_t)IMAGE_LEN, (size_t)HDR_LEN, sizeof(served_module));
	return 0;
}
