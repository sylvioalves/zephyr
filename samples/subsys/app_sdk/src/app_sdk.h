/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SDK_H_
#define APP_SDK_H_

#include <stdint.h>
#include <stddef.h>

/*
 * App SDK - the entire platform surface an application may use. An app codes
 * against ONLY this header. It never sees syscalls, brokers, drivers,
 * devicetree, or kernel objects. The firmware implements each app_* function
 * (forwarding to the real service) and exports it; a loadable module resolves
 * these symbols at load time, so the export set is both the API and the
 * security boundary - anything not here is uncallable.
 *
 * The line between an app CAPABILITY and SYSTEM ADMINISTRATION is explicit:
 * apps may publish/subscribe MQTT (app-level behavior), but apps may NOT change
 * Wi-Fi credentials (privileged config affecting the whole device). Admin calls
 * return -EPERM unless the caller holds the admin capability.
 */

/* ---- logging / time (always available) ---- */
void     app_log(const char *msg);
void     app_sleep_ms(uint32_t ms);
uint32_t app_uptime_ms(void);

/* ---- sensors (read-only, shared, any app) ---- */
int app_sensor_read(const char *name, int32_t *milli_value);

/* ---- simple I/O ---- */
int app_led_set(int on);

/* ---- MQTT (app-level capability: an app's own pub/sub) ---- */
int app_mqtt_publish(const char *topic, const char *payload);
/* Returns 0 and fills topic/payload on a received message, -EAGAIN if none. */
int app_mqtt_poll(char *topic, size_t topic_sz, char *payload, size_t payload_sz,
		  int32_t timeout_ms);

/* ---- system administration (privileged: NOT a normal app capability) ----
 * These affect the whole device. They succeed only for an app granted the
 * admin capability; a normal app gets -EPERM.
 */
int app_admin_wifi_set_credentials(const char *ssid, const char *psk);
int app_admin_wifi_forget(const char *ssid);

#endif /* APP_SDK_H_ */
