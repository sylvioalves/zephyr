/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_dispatch.h>
#include <errno.h>
#include <stdio.h>
#include <zephyr/llext/symbol.h>

/* The served service UUID and the two characteristics under it, as
 * little-endian bytes. The host grants the service UUID to this application,
 * so it must match the one in src/main.c.
 */
static const uint8_t svc_uuid[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x44, 0x44, 0xa0, 0xc3, 0xb2, 0xa1,
};
static const uint8_t counter_uuid[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x44, 0x44, 0xa1, 0xc3, 0xb2, 0xa1,
};
static const uint8_t inbox_uuid[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x44, 0x44, 0xa2, 0xc3, 0xb2, 0xa1,
};

static void on_write(const struct app_event *evt, void *user)
{
	int *n = user;
	uint8_t buf[8];
	char line[64];

	(*n)++;
	int got = app_svc_ble_serve_get((int)evt->line, buf, sizeof(buf));

	(void)snprintf(line, sizeof(line), "llext-gatt: peer wrote 0x%02x (#%d)",
		       (got > 0) ? buf[0] : 0, *n);
	app_svc_log(line);
}

void app_main(void *arg)
{
	(void)arg;

	struct app_dispatch d = {0};
	int writes = 0;
	struct app_ble_chrc chrc[2] = {
		{.uuid_len = 16, .readable = 1, .writable = 0},
		{.uuid_len = 16, .readable = 1, .writable = 1},
	};

	for (int i = 0; i < 16; i++) {
		chrc[0].uuid[i] = counter_uuid[i];
		chrc[1].uuid[i] = inbox_uuid[i];
	}

	if (app_svc_ble_serve_start("c5-llext", svc_uuid, sizeof(svc_uuid), chrc,
				    ARRAY_SIZE(chrc)) != 0) {
		app_svc_log("llext-gatt: serve failed");
		return;
	}
	app_svc_log("llext-gatt: serving from a loadable module");

	(void)app_dispatch_on(&d, APP_EVENT_GATT_WRITE, on_write, &writes);

	for (uint8_t sec = 0;; sec++) {
		uint8_t v[4] = {sec, 0, 0, 0};

		(void)app_svc_ble_serve_set(0, v, sizeof(v));
		(void)app_dispatch_run(&d, 1000);
	}
}
EXPORT_SYMBOL(app_main);
