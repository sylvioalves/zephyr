/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The built-in app catalog. Each app runs in user mode and uses only services. */

#include "platform.h"
#include "svc.h"

#include <zephyr/kernel.h>

/* thermostat: reads the temperature and stays alive. */
static void app_thermostat(void *arg)
{
	ARG_UNUSED(arg);
	for (int i = 0; ; i++) {
		int32_t t;

		if (svc_sensor_read(&t) == 0) {
			svc_log("thermostat: read temperature");
		}
		svc_heartbeat();
		k_msleep(1000);
	}
}

/* logger: just stays alive, proving liveness. */
static void app_logger(void *arg)
{
	ARG_UNUSED(arg);
	for (int i = 0; ; i++) {
		svc_log("logger: tick");
		svc_heartbeat();
		k_msleep(1000);
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

/* The catalog: name -> built-in entry. A loadable app would add an entry with
 * source APP_LOADABLE and an entry resolved from its llext at install time.
 */
static const struct app_def catalog[] = {
	{ "thermostat", APP_BUILTIN, app_thermostat },
	{ "logger",     APP_BUILTIN, app_logger },
	{ "buggy",      APP_BUILTIN, app_buggy },
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
