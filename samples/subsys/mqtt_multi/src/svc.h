/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_MULTI_SVC_H_
#define MQTT_MULTI_SVC_H_

#include <zephyr/kernel.h>

/*
 * Brokered MQTT for isolated apps. The Wi-Fi and network stacks live once in the
 * supervisor; each app gets its own MQTT session there but holds no socket and no
 * stack of its own. An app reaches the broker only through these calls.
 */
__syscall void svc_log(const char *msg);

/* Open this app's MQTT session (its client id and publish topic). */
__syscall int svc_mqtt_open(const char *client_id, const char *topic);

/* Publish a payload on this app's session. */
__syscall int svc_mqtt_publish(const char *payload);

#include <zephyr/syscalls/svc.h>

#endif /* MQTT_MULTI_SVC_H_ */
