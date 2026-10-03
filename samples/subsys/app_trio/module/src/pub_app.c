/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App 1 (publisher): connect to the broker and publish the SoC die temperature
 * every five seconds on the shared topic. An isolated user-mode app reaching
 * the system only through service syscalls.
 */

#include <zephyr/app_platform/app_sdk.h>

static void fmt_temp(int milli, char *out)
{
	int whole = milli / 1000;
	int tenth = (milli % 1000) / 100;
	int i = 0;

	if (whole < 0) {
		out[i++] = '-';
		whole = -whole;
	}
	if (whole >= 10) {
		out[i++] = '0' + (whole / 10);
	}
	out[i++] = '0' + (whole % 10);
	out[i++] = '.';
	out[i++] = '0' + tenth;
	out[i] = '\0';
}

void app_main(void *arg)
{
	int tries = 0;

	ARG_UNUSED(arg);

	app_svc_log("pub: waiting for the shared network link");
	if (app_svc_net_ready() != 0) {
		app_svc_log("pub: network not ready");
		return;
	}

	struct app_svc_wifi_status ws;

	if (app_svc_wifi_status(&ws) == 0) {
		char line[80];
		int i = 0;
		const char *p = "pub: link ";

		while (*p) {
			line[i++] = *p++;
		}
		p = ws.connected ? "up on " : "down ";
		while (*p) {
			line[i++] = *p++;
		}
		p = ws.ssid;
		while (*p) {
			line[i++] = *p++;
		}
		line[i++] = ' '; line[i++] = '(';
		p = ws.ipv4;
		while (*p) {
			line[i++] = *p++;
		}
		line[i++] = ')';
		line[i] = '\0';
		app_svc_log(line);
	}

	/* A public broker may drop a connection attempt: try a few times. */
	while (app_svc_mqtt_open("zephyr-trio-pub", "zephyr/trio/temp",
				 "zephyr/trio/unused") != 0) {
		if (++tries == 5) {
			app_svc_log("pub: MQTT session failed");
			return;
		}
		app_svc_log("pub: MQTT session failed, retrying");
	}
	app_svc_log("pub: publishing die temp every 5s on zephyr/trio/temp");

	for (;;) {
		char payload[8];

		fmt_temp(app_svc_die_temp_milli(), payload);
		app_svc_mqtt_publish(payload);
		app_sleep_ms(5000);
	}
}
EXPORT_SYMBOL(app_main);
