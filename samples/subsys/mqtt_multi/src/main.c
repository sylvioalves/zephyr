/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two MQTT apps at once over one shared stack. The supervisor brings up Wi-Fi
 * once, then launches two isolated user-mode apps; each opens its own brokered
 * MQTT session and publishes to its own topic. The Wi-Fi, network and MQTT code
 * is linked once, in the supervisor; the apps are tiny because they only make
 * svc_mqtt_* calls.
 */

#include "svc.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern int net_link_up(void);
extern void app_sensor(void *arg);
extern void app_status(void *arg);

int main(void)
{
	printk("\n*** two isolated MQTT apps on one shared stack (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	if (net_link_up() != 0) {
		return 0;
	}

	const struct app_def sensor = {
		"sensor-app", APP_BUILTIN, app_sensor, 0, false,
	};
	const struct app_def status = {
		"status-app", APP_BUILTIN, app_status, 0, false,
	};

	app_platform_launch(&sensor);
	app_platform_launch(&status);

	printk("both apps launched - each publishing on its own topic\n");
	k_msleep(20000);

	printk("\n*** done: two isolated apps shared one network stack ***\n");
	return 0;
}
