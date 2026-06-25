/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App SDK implementation: each app_* function forwards to the real platform
 * service and is exported so loadable modules can resolve it at load time. This
 * file is the one place the platform's API surface is defined and gated.
 */

#include "app_sdk.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/llext/symbol.h>
#include <errno.h>
#include <string.h>

/* The admin capability: set by the supervisor for trusted apps only. */
static bool caller_is_admin;

void app_sdk_set_admin(bool admin)
{
	caller_is_admin = admin;
}

/* ---- logging / time ---- */

void app_log(const char *msg)
{
	printk("    [app] %s\n", msg);
}
EXPORT_SYMBOL(app_log);

void app_sleep_ms(uint32_t ms)
{
	k_msleep((int32_t)ms);
}
EXPORT_SYMBOL(app_sleep_ms);

uint32_t app_uptime_ms(void)
{
	return (uint32_t)k_uptime_get();
}
EXPORT_SYMBOL(app_uptime_ms);

/* ---- sensors: forward to the sensor catalog (read-only, shareable) ---- */

extern int sensor_catalog_lookup(const char *name);
extern int sensor_catalog_read(int idx, int32_t *milli_value);

int app_sensor_read(const char *name, int32_t *milli_value)
{
	int idx = sensor_catalog_lookup(name);

	if (idx < 0) {
		return -ENODEV;
	}
	return sensor_catalog_read(idx, milli_value);
}
EXPORT_SYMBOL(app_sensor_read);

/* ---- LED ---- */

extern int board_led_set(int on);

int app_led_set(int on)
{
	return board_led_set(on);
}
EXPORT_SYMBOL(app_led_set);

/* ---- MQTT: app-level capability, brokered ---- */

extern int mqtt_broker_publish(const char *topic, const char *payload);
extern int mqtt_broker_poll(char *topic, size_t tsz, char *payload, size_t psz,
			    int32_t timeout_ms);

int app_mqtt_publish(const char *topic, const char *payload)
{
	return mqtt_broker_publish(topic, payload);
}
EXPORT_SYMBOL(app_mqtt_publish);

int app_mqtt_poll(char *topic, size_t topic_sz, char *payload, size_t payload_sz,
		  int32_t timeout_ms)
{
	return mqtt_broker_poll(topic, topic_sz, payload, payload_sz, timeout_ms);
}
EXPORT_SYMBOL(app_mqtt_poll);

/* ---- system administration: privileged, gated ---- */

extern int wifi_admin_set_credentials(const char *ssid, const char *psk);
extern int wifi_admin_forget(const char *ssid);

int app_admin_wifi_set_credentials(const char *ssid, const char *psk)
{
	if (!caller_is_admin) {
		printk("    [sdk] DENIED: app lacks admin capability for wifi config\n");
		return -EPERM;
	}
	return wifi_admin_set_credentials(ssid, psk);
}
EXPORT_SYMBOL(app_admin_wifi_set_credentials);

int app_admin_wifi_forget(const char *ssid)
{
	if (!caller_is_admin) {
		return -EPERM;
	}
	return wifi_admin_forget(ssid);
}
EXPORT_SYMBOL(app_admin_wifi_forget);
