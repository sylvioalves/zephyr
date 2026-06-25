/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Platform services the SDK forwards to. Real services would each be a broker;
 * here they are compact stand-ins that show the shape: sensors are read live,
 * the LED toggles a pin, MQTT is a brokered queue (a real client would connect
 * over the network), and Wi-Fi credential management is privileged config
 * persisted with the wifi_credentials subsystem.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <string.h>
#include <errno.h>

/* ---- sensors ---- */

static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));

int sensor_catalog_lookup(const char *name)
{
	if (strcmp(name, "temperature") == 0) {
		return 0;
	}
	return -1;
}

int sensor_catalog_read(int idx, int32_t *milli_value)
{
	if (idx != 0 || !device_is_ready(die_temp)) {
		return -ENODEV;
	}
	if (sensor_sample_fetch(die_temp) != 0) {
		return -EIO;
	}
	struct sensor_value v;

	if (sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v) != 0) {
		return -EIO;
	}
	*milli_value = v.val1 * 1000 + v.val2 / 1000;
	return 0;
}

/* ---- LED ---- */

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(sdk_led), gpios);

int board_led_set(int on)
{
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}
	return gpio_pin_set_dt(&led, on ? 1 : 0);
}

/* ---- MQTT broker (queue stand-in for a network client) ---- */

struct mqtt_msg {
	char topic[32];
	char payload[64];
};

K_MSGQ_DEFINE(mqtt_inbox, sizeof(struct mqtt_msg), 4, 4);

int mqtt_broker_publish(const char *topic, const char *payload)
{
	/* A real broker forwards to mqtt_publish() over the network. For the
	 * demo, loop it back so a subscriber can observe delivery end to end.
	 */
	printk("    [mqtt] publish topic='%s' payload='%s'\n", topic, payload);

	struct mqtt_msg m;

	strncpy(m.topic, topic, sizeof(m.topic) - 1);
	m.topic[sizeof(m.topic) - 1] = '\0';
	strncpy(m.payload, payload, sizeof(m.payload) - 1);
	m.payload[sizeof(m.payload) - 1] = '\0';
	(void)k_msgq_put(&mqtt_inbox, &m, K_NO_WAIT);
	return 0;
}

int mqtt_broker_poll(char *topic, size_t tsz, char *payload, size_t psz,
		     int32_t timeout_ms)
{
	struct mqtt_msg m;

	if (k_msgq_get(&mqtt_inbox, &m, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
	}
	strncpy(topic, m.topic, tsz - 1);
	topic[tsz - 1] = '\0';
	strncpy(payload, m.payload, psz - 1);
	payload[psz - 1] = '\0';
	return 0;
}

/* ---- privileged Wi-Fi credential management ---- */

int wifi_admin_set_credentials(const char *ssid, const char *psk)
{
	/* A real implementation calls wifi_credentials_set_personal() to persist
	 * to flash; shown here as the privileged operation it is.
	 */
	printk("    [admin] Wi-Fi credentials updated: ssid='%s' (psk %zu chars)\n",
	       ssid, strlen(psk));
	return 0;
}

int wifi_admin_forget(const char *ssid)
{
	printk("    [admin] Wi-Fi network forgotten: ssid='%s'\n", ssid);
	return 0;
}
