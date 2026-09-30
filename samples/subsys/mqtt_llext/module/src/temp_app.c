/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable die-temperature reporter, built as an llext. It waits for the
 * platform's shared network link, opens its own MQTT session, subscribes to a
 * command topic and publishes the SoC die temperature every ten seconds. The
 * Wi-Fi, socket, MQTT client and sensor all live in the platform's shared
 * service library, so this module links none of them and stays small.
 */

#include <zephyr/app_platform/app_sdk.h>

/* Format milli-degrees as "DD.d" without pulling printf into the module. */
static void fmt_temp(int milli, char *out)
{
	int whole = milli / 1000;
	int tenth = (milli % 1000) / 100;
	int i = 0;

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
	ARG_UNUSED(arg);

	app_svc_log("temp-app: waiting for the shared network link");
	if (app_svc_net_ready() != 0) {
		app_svc_log("temp-app: network not ready");
		return;
	}

	app_svc_log("temp-app: opening MQTT session");
	if (app_svc_mqtt_open("zephyr-temp", "zephyr/c6/temp", "zephyr/c6/cmd") != 0) {
		app_svc_log("temp-app: session failed");
		return;
	}
	app_svc_log("temp-app: publishing die temp every 10s");

	for (;;) {
		char payload[8];

		fmt_temp(app_svc_die_temp_milli(), payload);
		app_svc_mqtt_publish(payload);
		app_svc_heartbeat();
		app_sleep_ms(10000);
	}
}
EXPORT_SYMBOL(app_main);
