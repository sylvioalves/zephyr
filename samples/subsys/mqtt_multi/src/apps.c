/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two isolated apps, each with its own MQTT identity and topic, both publishing
 * through the one shared network stack in the supervisor. Neither app links the
 * Wi-Fi, TCP, socket or MQTT code: it only makes svc_mqtt_* calls.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <stdio.h>

void app_sensor(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("sensor-app: opening own MQTT session");
	if (svc_mqtt_open("zephyr-sensor", "zephyr/c6/sensor") != 0) {
		svc_log("sensor-app: session failed");
		return;
	}

	for (int i = 0; ; i++) {
		char msg[32];

		snprintf(msg, sizeof(msg), "temp=%d", 20 + (i % 5));
		svc_mqtt_publish(msg);
		k_msleep(3000);
	}
}

void app_status(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("status-app: opening own MQTT session");
	if (svc_mqtt_open("zephyr-status", "zephyr/c6/status") != 0) {
		svc_log("status-app: session failed");
		return;
	}

	for (int i = 0; ; i++) {
		char msg[32];

		snprintf(msg, sizeof(msg), "uptime=%d", i * 4);
		svc_mqtt_publish(msg);
		k_msleep(4000);
	}
}
