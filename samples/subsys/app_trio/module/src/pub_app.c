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

#include <zephyr/app_platform/app_services.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

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
	ARG_UNUSED(arg);

	svc_log("pub: waiting for the shared network link");
	if (svc_net_ready() != 0) {
		svc_log("pub: network not ready");
		return;
	}
	if (svc_mqtt_open("zephyr-trio-pub", "zephyr/trio/temp",
			  "zephyr/trio/unused") != 0) {
		svc_log("pub: MQTT session failed");
		return;
	}
	svc_log("pub: publishing die temp every 5s on zephyr/trio/temp");

	for (;;) {
		char payload[8];

		fmt_temp(svc_die_temp_milli(), payload);
		svc_mqtt_publish(payload);
		k_msleep(5000);
	}
}
EXPORT_SYMBOL(app_main);
