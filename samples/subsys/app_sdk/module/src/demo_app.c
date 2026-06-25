/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A loadable application. It is compiled separately from the firmware and knows
 * ONLY app_sdk.h - no kernel headers, no drivers, no brokers. At load time its
 * app_* calls resolve against the firmware's exported SDK symbols. It can call
 * nothing the platform did not export.
 */

#include "app_sdk.h"
#include <zephyr/llext/symbol.h>

void app_main(void)
{
	app_log("loaded app starting (knows only app_sdk.h)");

	/* Read a sensor by name - no idea it is an on-chip I2C-like device. */
	int32_t temp;

	if (app_sensor_read("temperature", &temp) == 0) {
		char buf[48];

		/* (no snprintf in this tiny module; app_log a fixed message) */
		app_log("read temperature ok");
		app_led_set(temp > 25000);
		(void)buf;
	}

	/* Publish app data over MQTT (an app-level capability). */
	app_mqtt_publish("devices/app1/temp", "25.6");

	/* Receive a message (loopback in the demo broker). */
	char topic[32], payload[64];

	if (app_mqtt_poll(topic, sizeof(topic), payload, sizeof(payload), 500) == 0) {
		app_log("mqtt received a message");
	}

	/* Try a privileged admin operation - this app is NOT admin, so the
	 * platform must deny it. This is the capability boundary in action.
	 */
	int rc = app_admin_wifi_set_credentials("HomeNet", "secret123");

	if (rc == -1 || rc < 0) {
		app_log("wifi-config attempt was DENIED (not an admin app) - good");
	}

	app_log("loaded app done");
}
EXPORT_SYMBOL(app_main);
