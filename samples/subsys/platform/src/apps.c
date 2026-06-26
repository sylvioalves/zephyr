/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The built-in app catalog. Each app runs in user mode and reaches the platform
 * only through service syscalls, and only the services its capabilities grant.
 */

#include "app_caps.h"
#include "svc.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>
#include <string.h>

/* climate: granted the sensor + PWM. Reads temperature and drives a fan PWM. */
static void app_climate(void *arg)
{
	ARG_UNUSED(arg);
	uint8_t duty = 0;

	for (;;) {
		int32_t t;

		if (svc_sensor_read(&t) == 0) {
			svc_log("climate: read temperature, adjusting fan");
		}
		svc_pwm_set(duty);
		duty = (duty + 25) % 125;
		svc_heartbeat();
		k_msleep(1000);
	}
}

/* datalogger: granted flash + RTC. Stamps a record and reads it back. */
static void app_datalogger(void *arg)
{
	ARG_UNUSED(arg);

	for (uint32_t n = 0; ; n++) {
		uint32_t now = 0;

		if (svc_rtc_now(&now) == 0) {
			uint8_t rec[8];

			memcpy(rec, &now, 4);
			memcpy(rec + 4, &n, 4);
			if (svc_flash_write(0, rec, sizeof(rec)) == sizeof(rec)) {
				svc_log("datalogger: wrote timestamped record");
			}
			uint8_t back[8] = { 0 };

			svc_flash_read(0, back, sizeof(back));
		}
		svc_heartbeat();
		k_msleep(1000);
	}
}

/* blinker: granted GPIO only. Blinks the LED, and probes a denied service. */
static void app_blinker(void *arg)
{
	ARG_UNUSED(arg);
	int on = 0;
	bool probed = false;

	for (;;) {
		svc_led_set(on);
		on = !on;

		if (!probed) {
			int32_t t;
			int rc = svc_sensor_read(&t);   /* not granted CAP_SENSOR */

			if (rc == -EPERM) {
				svc_log("blinker: sensor denied (-EPERM), as expected");
			}
			probed = true;
		}
		svc_heartbeat();
		k_msleep(500);
	}
}

/* buggy: stops feeding its watchdog -> the platform reclaims it. */
static void app_buggy(void *arg)
{
	ARG_UNUSED(arg);
	svc_log("buggy: about to hang (will be reclaimed)");
	for (;;) {
		/* no heartbeat, no yield */
	}
}

/*
 * The catalog: name -> built-in entry + granted capabilities. A loadable app
 * would add an entry with source APP_LOADABLE and a caps mask from its manifest.
 */
static const struct app_def catalog[] = {
	{ "climate",    APP_BUILTIN, app_climate,    CAP_SENSOR | CAP_PWM },
	{ "datalogger", APP_BUILTIN, app_datalogger, CAP_FLASH | CAP_RTC },
	{ "blinker",    APP_BUILTIN, app_blinker,    CAP_GPIO },
	{ "buggy",      APP_BUILTIN, app_buggy,      0 },
};

const struct app_def *platform_catalog_find(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(catalog); i++) {
		if (strcmp(catalog[i].name, name) == 0) {
			return &catalog[i];
		}
	}
	return NULL;
}
