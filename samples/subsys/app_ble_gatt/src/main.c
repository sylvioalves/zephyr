/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application using the generic GATT client. The
 * application names a service and a characteristic by UUID and the platform
 * connects, discovers and performs the exchange; no connection or attribute
 * handle ever reaches the application, so it can only address what it names.
 *
 * The application is granted the peer's sensor service and not its private
 * service. Reading the granted service works, writing it works, and a read of
 * the ungranted service is refused with -EACCES by the platform before any
 * GATT traffic is generated. That split between holding APP_CAP_BLE and being
 * granted a service is the point of the sample.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

#define PEER_NAME "gatt-peer"
#define GATT_TIMEOUT_MS 8000U

/* UUIDs in little-endian byte order, as the service expects them. */
static const uint8_t sensor_svc[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x22, 0x22, 0xe0, 0xc3, 0xb2, 0xa1,
};
static const uint8_t sensor_val[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x22, 0x22, 0xe1, 0xc3, 0xb2, 0xa1,
};
static const uint8_t sensor_cfg[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x22, 0x22, 0xe2, 0xc3, 0xb2, 0xa1,
};
static const uint8_t private_svc[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x33, 0x33, 0xf0, 0xc3, 0xb2, 0xa1,
};
static const uint8_t private_val[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x33, 0x33, 0xf1, 0xc3, 0xb2, 0xa1,
};

static void gatt_app(void *arg)
{
	ARG_UNUSED(arg);

	uint8_t buf[16];
	char line[64];

	app_svc_log("gatt: reading the granted sensor value");
	int n = app_svc_ble_gatt_read(PEER_NAME, sensor_svc, sizeof(sensor_svc),
				      sensor_val, sizeof(sensor_val), buf,
				      sizeof(buf), GATT_TIMEOUT_MS);

	if (n < 0) {
		(void)snprintk(line, sizeof(line), "gatt: read failed (%d)", n);
		app_svc_log(line);
		return;
	}
	(void)snprintk(line, sizeof(line),
		       "gatt: read %d byte(s), first 0x%02x", n, buf[0]);
	app_svc_log(line);

	uint8_t cfg[2] = { 0x5A, 0xA5 };
	int rc = app_svc_ble_gatt_write(PEER_NAME, sensor_svc, sizeof(sensor_svc),
					sensor_cfg, sizeof(sensor_cfg), cfg,
					sizeof(cfg), GATT_TIMEOUT_MS);

	if (rc == 0) {
		app_svc_log("gatt: wrote the granted config value");
	} else {
		(void)snprintk(line, sizeof(line), "gatt: write failed (%d)", rc);
		app_svc_log(line);
	}

	/* The peer exposes this service, but it was never granted here. The
	 * platform must refuse it without going on air.
	 */
	rc = app_svc_ble_gatt_read(PEER_NAME, private_svc, sizeof(private_svc),
				   private_val, sizeof(private_val), buf,
				   sizeof(buf), GATT_TIMEOUT_MS);
	if (rc == -EACCES) {
		app_svc_log("gatt: ungranted service correctly denied");
	} else {
		(void)snprintk(line, sizeof(line),
			       "gatt: ERROR ungranted service returned %d", rc);
		app_svc_log(line);
	}

	app_svc_log("gatt: done");
}

static void nogatt_app(void *arg)
{
	ARG_UNUSED(arg);

	uint8_t buf[8];

	if (app_svc_ble_gatt_read(PEER_NAME, sensor_svc, sizeof(sensor_svc),
				  sensor_val, sizeof(sensor_val), buf, sizeof(buf),
				  1000U) == -EPERM) {
		app_svc_log("nogatt: read denied without the capability");
	} else {
		app_svc_log("nogatt: ERROR read was allowed");
	}
}

int main(void)
{
	printk("\n*** app_ble_gatt: GATT by UUID through a platform service ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "gatt",
		.source = APP_BUILTIN,
		.entry = gatt_app,
		.caps = APP_CAP_LOG | APP_CAP_BLE,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}

	/* Only the sensor service; the peer's private service stays out of
	 * reach even though the application holds APP_CAP_BLE.
	 */
	app_platform_grant_ble_gatt(id, sensor_svc, sizeof(sensor_svc));

	struct app_def denied = {
		.name = "nogatt",
		.source = APP_BUILTIN,
		.entry = nogatt_app,
		.caps = APP_CAP_LOG,
	};

	if (app_platform_launch(&denied) < 0) {
		printk("ungranted launch failed\n");
		return 0;
	}

	printk("*** launched isolated gatt app (group %d) ***\n", id);
	return 0;
}
