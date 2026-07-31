/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PLATFORM_APP_SDK_H_
#define APP_PLATFORM_APP_SDK_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stddef.h>

/*
 * The unified platform SDK - the single surface an application uses. It pulls
 * together the patterns proven across this series into one coherent API:
 *
 *   - app runtime: an app is a group of threads sharing one isolated domain;
 *     app_thread_spawn adds a thread to the calling app.
 *   - service discovery: app_service_count / app_service_info let an app learn
 *     what the platform offers at runtime (the registry).
 *   - three capability tiers:
 *       open       - any app may use (sensors: read-only, shared)
 *       scoped     - granted per app by manifest (i2c: only a (bus,addr) slice)
 *       admin      - system only (wifi credentials): -EPERM otherwise
 *
 * An app codes against this header alone. The export set is the API and the
 * security boundary; capability tier decides who may call what.
 */

/* App thread entry point. */
typedef void (*app_entry_t)(void *arg);

/* ---- runtime ---- */
__syscall void     app_log(const char *msg);
__syscall void     app_sleep_ms(uint32_t ms);
__syscall uint32_t app_uptime_ms(void);
__syscall int      app_thread_spawn(app_entry_t entry, void *arg);
__syscall int      app_thread_join(int handle);

/* ---- service discovery (the registry, queryable) ---- */
__syscall int app_service_count(void);
/* tier: 0=open, 1=scoped, 2=admin */
__syscall int app_service_info(int idx, char *name, size_t name_sz, int *tier);

/* ---- open capability: sensors ---- */
__syscall int app_sensor_read(const char *name, int32_t *milli_value);

/* ---- scoped capability: i2c (manifest grants a (bus,addr) slice) ---- */
__syscall int app_i2c_read(uint8_t bus, uint16_t addr, uint8_t reg);

/* ---- admin capability: system configuration (denied to normal apps) ---- */
__syscall int app_admin_wifi_set(const char *ssid, const char *psk);

#include <zephyr/syscalls/app_sdk.h>

#endif /* APP_PLATFORM_APP_SDK_H_ */
