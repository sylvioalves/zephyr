/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_APP_PLATFORM_APP_EVENTS_H_
#define ZEPHYR_SUBSYS_APP_PLATFORM_APP_EVENTS_H_

#include <zephyr/app_platform/app_services.h>
#include <zephyr/kernel.h>

/* Producer side of the per-application event queues. Input services call these
 * to deliver an event; they are safe from an interrupt handler. Internal to the
 * subsystem and not part of the public API.
 */

#ifdef CONFIG_APP_PLATFORM_EVENTS
void app_events_post(int id, const struct app_event *evt);
void app_events_broadcast(const struct app_event *evt, bool (*pred)(int id));
#else
static inline void app_events_post(int id, const struct app_event *evt)
{
	ARG_UNUSED(id);
	ARG_UNUSED(evt);
}
static inline void app_events_broadcast(const struct app_event *evt,
					bool (*pred)(int id))
{
	ARG_UNUSED(evt);
	ARG_UNUSED(pred);
}
#endif

#ifdef CONFIG_APP_PLATFORM_BLE_GATT
/* Grant table lives in app_ble.c; the server side checks it too. */
bool app_platform_ble_gatt_granted(int id, const uint8_t *uuid, size_t uuid_len);
#endif

#endif /* ZEPHYR_SUBSYS_APP_PLATFORM_APP_EVENTS_H_ */
