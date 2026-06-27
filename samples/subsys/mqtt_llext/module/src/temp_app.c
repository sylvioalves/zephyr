/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable die-temperature reporter, built as an llext. It subscribes to a
 * command topic and publishes the SoC die temperature every ten seconds, all
 * through brokered svc_ calls. It links no Wi-Fi, socket or MQTT code, so the
 * loadable module is only a few hundred bytes.
 */

#include "svc.h"

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

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

	svc_log("temp-app: opening MQTT session");
	if (svc_mqtt_open("zephyr-temp", "zephyr/c6/temp", "zephyr/c6/cmd") != 0) {
		svc_log("temp-app: session failed");
		return;
	}
	svc_log("temp-app: publishing die temp every 10s");

	for (;;) {
		char payload[8];

		fmt_temp(svc_die_temp_milli(), payload);
		svc_mqtt_publish(payload);
		svc_heartbeat();
		k_msleep(10000);
	}
}
EXPORT_SYMBOL(app_main);
