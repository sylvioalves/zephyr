/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_LLEXT_SVC_H_
#define MQTT_LLEXT_SVC_H_

#include <zephyr/kernel.h>

/*
 * Services available to the loadable app. The Wi-Fi, socket, MQTT client and
 * die-temperature sensor all live in the supervisor; the app reaches them only
 * through these calls, so it carries none of that code and stays tiny.
 */
__syscall void svc_log(const char *msg);
__syscall void svc_heartbeat(void);

/* Open this app's MQTT session with a publish topic and a subscribe topic. */
__syscall int svc_mqtt_open(const char *client_id, const char *pub_topic,
			    const char *sub_topic);

/* Publish a payload on this app's session. */
__syscall int svc_mqtt_publish(const char *payload);

/* Read the SoC die temperature in milli-degrees Celsius. */
__syscall int svc_die_temp_milli(void);

#include <zephyr/syscalls/svc.h>

#endif /* MQTT_LLEXT_SVC_H_ */
