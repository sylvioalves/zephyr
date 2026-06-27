/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_SERVICES_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_SERVICES_H_

#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_caps.h>

/**
 * @brief Application platform shared services
 * @defgroup app_platform_services Application platform shared services
 * @ingroup app_platform
 * @{
 *
 * Typed, capability-gated services that an application reaches as ordinary
 * syscalls. The implementation lives once in the platform, so a built-in or a
 * loadable application calls these without linking any driver code; a loadable
 * application that calls only these stays a few kilobytes.
 *
 * Every service enforces its capability bit (see @ref app_platform_caps) at the
 * syscall boundary and returns -EPERM to an ungranted caller. Each service is
 * written to be safe under concurrent calls from several isolated applications:
 * it is either stateless, keyed by the calling application's group id, or
 * serialized internally.
 */

/** Log a line, prefixed with the calling application's group id. */
__syscall void svc_log(const char *msg);

/** Feed the calling application's liveness watchdog (if monitored). */
__syscall void svc_heartbeat(void);

/**
 * @brief Compute a SHA-256 digest. Needs @ref APP_CAP_CRYPTO.
 *
 * Stateless: safe to call concurrently from any number of applications.
 *
 * @param in    Input buffer.
 * @param len   Input length (bounded by the platform).
 * @param out32 Receives the 32-byte digest.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_sha256(const uint8_t *in, size_t len, uint8_t *out32);

/**
 * @brief Fill a buffer with random bytes. Needs @ref APP_CAP_RANDOM.
 *
 * Stateless: safe under concurrent callers.
 *
 * @param buf Receives the random bytes.
 * @param len Number of bytes (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_random(uint8_t *buf, size_t len);

/**
 * @brief Milliseconds since boot. Needs @ref APP_CAP_TIME.
 *
 * Stateless.
 */
__syscall uint32_t svc_uptime_ms(void);

/**
 * @brief Free-running CPU cycle count. Needs @ref APP_CAP_TIME.
 *
 * Stateless.
 */
__syscall uint32_t svc_cycles(void);

/**
 * @brief Read the SoC die temperature in milli-degrees Celsius.
 * Needs @ref APP_CAP_SENSOR.
 *
 * Stateless: a fetch-and-read of a shared sensor, safe under concurrent callers.
 */
__syscall int svc_die_temp_milli(void);

/**
 * @brief Store a value under a key. Needs @ref APP_CAP_KV.
 *
 * Lock-serialized: the store is shared across applications, so concurrent
 * set/get calls are serialized internally. Keys are namespaced per application,
 * so one application cannot read or overwrite another's keys.
 *
 * @param key   Key string (bounded by the platform).
 * @param value Value to store.
 * @retval 0 on success, -EPERM without the capability, -ENOSPC if full.
 */
__syscall int svc_kv_set(const char *key, uint32_t value);

/**
 * @brief Read a value by key. Needs @ref APP_CAP_KV.
 *
 * Lock-serialized; reads only the calling application's namespace.
 *
 * @param key Key string.
 * @param out Receives the value.
 * @retval 0 on success, -EPERM without the capability, -ENOENT if absent.
 */
__syscall int svc_kv_get(const char *key, uint32_t *out);

/**
 * @brief Read from the raw storage partition. Needs @ref APP_CAP_FLASH.
 *
 * Lock-serialized: the partition is a shared peripheral, so accesses are
 * serialized and every offset and length is bounds-checked against the
 * partition.
 *
 * @param off Offset within the storage partition.
 * @param buf Receives the bytes.
 * @param len Number of bytes (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_flash_read(uint32_t off, uint8_t *buf, size_t len);

/**
 * @brief Bring up networking and wait until it is ready. Needs @ref APP_CAP_NET.
 *
 * Lock-serialized: the first caller brings up Wi-Fi and resolves the broker;
 * later callers (from other applications) wait for the same shared link. The
 * Wi-Fi and network stacks live once in the platform, so an application that
 * uses networking links none of that code.
 *
 * @retval 0 when networking is ready, -EPERM without the capability, negative
 *         errno on failure.
 */
__syscall int svc_net_ready(void);

/**
 * @brief Open the calling application's MQTT session. Needs @ref APP_CAP_NET.
 *
 * Per-app-keyed: each application gets its own session, keyed by its group id,
 * so applications publish under their own identity and cannot touch each other's
 * sessions. Call @ref svc_net_ready first.
 *
 * @param client_id Broker client id for this application.
 * @param pub_topic Topic this application publishes to.
 * @param sub_topic Topic this application subscribes to.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_mqtt_open(const char *client_id, const char *pub_topic,
			    const char *sub_topic);

/**
 * @brief Publish on the calling application's MQTT session. Needs @ref APP_CAP_NET.
 *
 * Per-app-keyed: publishes only on this application's own session.
 *
 * @param payload Payload string.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_mqtt_publish(const char *payload);

/**
 * @brief Number of GPIO lines the platform exposes. Needs @ref APP_CAP_GPIO.
 *
 * The lines come from the board's @c leds devicetree node.
 */
__syscall int svc_gpio_count(void);

/**
 * @brief Drive a GPIO line. Needs @ref APP_CAP_GPIO and a per-line grant.
 *
 * Per-line grant: holding @ref APP_CAP_GPIO is not enough; the application is
 * granted a specific set of lines (see @ref app_platform_grant_gpio) and a call
 * to an ungranted line is refused. Stateless beyond that check.
 *
 * @param line Line index in [0, @ref svc_gpio_count).
 * @param on   Non-zero to drive high.
 * @retval 0 on success, -EPERM without the capability or line grant.
 */
__syscall int svc_gpio_set(int line, int on);

/**
 * @brief Grant an application a set of GPIO lines.
 *
 * Called by the launcher (supervisor) before or after starting an application
 * to allow it to drive specific lines through @ref svc_gpio_set. Lines not in
 * @p line_mask are refused even if the application holds @ref APP_CAP_GPIO. This
 * is the second-level grant within a capability: the capability admits the
 * service, the mask scopes it to specific resources.
 *
 * @param id        Application group id.
 * @param line_mask Bitmask of allowed line indices.
 */
void app_platform_grant_gpio(int id, uint32_t line_mask);

#include <zephyr/syscalls/app_services.h>

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_SERVICES_H_ */
