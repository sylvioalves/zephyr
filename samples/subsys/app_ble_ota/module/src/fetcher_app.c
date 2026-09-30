/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The application that performs the update. It holds APP_CAP_BLE, so it is the
 * only thing on the device allowed to pull an image from a peer; it links none
 * of the BLE stack and never sees the bytes, which the platform writes straight
 * to flash. When the transfer is done it publishes the result on a topic the
 * supervisor is subscribed to, and the supervisor does the verifying.
 */

#include <zephyr/app_platform/app_sdk.h>

#include <zephyr/sys/util.h>

#ifndef OTA_PEER_NAME
#define OTA_PEER_NAME "weather"
#endif

/* Render a small unsigned decimal without pulling libc into the module. */
static char *put_uint(char *p, char *end, uint32_t v)
{
	char tmp[12];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + (v % 10U));
		v /= 10U;
	} while (v > 0U && n < (int)sizeof(tmp));
	while (n > 0 && p < end) {
		*p++ = tmp[--n];
	}
	return p;
}

static char *put_str(char *p, char *end, const char *s)
{
	while (*s != '\0' && p < end) {
		*p++ = *s++;
	}
	return p;
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);
	char line[96];

	app_svc_log("ble-ota: looking for the update peer...");

	/* Scan first, so a peer that is simply not there is distinguishable
	 * from one whose OTA service the device cannot drive.
	 */
	int seen = app_svc_ble_scan(OTA_PEER_NAME, 8000);

	app_svc_log(seen == 1 ? "ble-ota: peer is advertising"
			      : "ble-ota: peer NOT seen in scan");

	uint32_t len = 0;
	uint32_t t0 = app_svc_uptime_ms();
	int rc = app_svc_ble_ota_fetch(OTA_PEER_NAME, 20000, &len);
	uint32_t took = app_svc_uptime_ms() - t0;

	char *p = line;
	char *end = line + sizeof(line) - 1;

	if (rc == 0) {
		p = put_str(p, end, "ble-ota: fetched ");
		p = put_uint(p, end, len);
		p = put_str(p, end, " bytes in ");
		p = put_uint(p, end, took);
		p = put_str(p, end, " ms");
	} else {
		p = put_str(p, end, "ble-ota: fetch failed rc=-");
		p = put_uint(p, end, (uint32_t)(-rc));
	}
	*p = '\0';
	app_svc_log(line);

	/* The bytes are on flash now. The supervisor watches for them and does
	 * the verifying; this application never sees them and cannot vouch for
	 * them.
	 */
	for (;;) {
		app_sleep_ms(1000);
	}
}
EXPORT_SYMBOL(app_main);
