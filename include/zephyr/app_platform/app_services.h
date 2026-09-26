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
 * Every service that touches hardware or another application enforces its
 * capability bit (see @ref app_platform_caps) as its first act at the syscall
 * boundary, and returns -EPERM to an ungranted caller; the few that expose only
 * the calling application's own state or the platform's static topology say so
 * in their own documentation. A service that is additionally scoped to
 * individual resources - a GPIO line, an ADC channel, an I2C address - keeps
 * the two refusals apart: -EPERM says the capability itself was never granted,
 * -EACCES says the capability is held but this particular resource is not
 * assigned to the caller. Each service is written to be safe under concurrent
 * calls from several isolated applications: it is either stateless, keyed by
 * the calling application's group id, or serialized internally.
 */

/**
 * @brief Log a line, prefixed with the calling application's group id.
 *
 * Needs @ref APP_CAP_LOG. The line is dropped without it; a void service has
 * no way to report the denial, and an application that cannot log is not a
 * reason to fault it.
 */
__syscall void svc_log(const char *msg);

/**
 * @brief Feed the calling application's liveness watchdog (if monitored).
 *
 * Deliberately needs no capability: the heartbeat is how the supervisor learns
 * the application is still progressing, so gating it would let a provisioning
 * mistake be reported as a hang.
 */
__syscall void svc_heartbeat(void);

/**
 * @brief Report whether the platform asked this application to exit.
 *
 * Needs no capability. An application that sees it should release what it
 * holds and return from its entry point, see @ref app_platform_request_stop.
 *
 * @retval 1 A stop was requested.
 * @retval 0 No stop was requested.
 */
__syscall int svc_stop_requested(void);

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
 *
 * @return Milliseconds since boot, or zero without the capability, which is
 *         indistinguishable from a genuine reading taken at boot.
 */
__syscall uint32_t svc_uptime_ms(void);

/**
 * @brief Free-running CPU cycle count. Needs @ref APP_CAP_TIME.
 *
 * Stateless.
 *
 * @return The cycle counter, or zero without the capability.
 */
__syscall uint32_t svc_cycles(void);

/**
 * @brief Read the SoC die temperature in milli-degrees Celsius.
 * Needs @ref APP_CAP_SENSOR.
 *
 * Stateless: a fetch-and-read of a shared sensor, safe under concurrent callers.
 *
 * @retval >=0 Temperature in millidegrees Celsius.
 * @retval -EPERM The application does not hold @ref APP_CAP_SENSOR.
 * @retval -EIO The sensor could not be read.
 * @retval -ENODEV The board exposes no die-temperature sensor.
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
 * @brief Forget one of the calling application's keys. Needs @ref APP_CAP_KV.
 *
 * Per-app-keyed: an application deletes only its own keys, and the freed slot
 * keeps no trace of what it held, so a later tenant of the slot cannot read it.
 *
 * @param key Key to remove.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_KV.
 * @retval -ENOENT when the application has no such key.
 * @retval -EBUSY if the store could not be locked.
 */
__syscall int svc_kv_delete(const char *key);

/**
 * @brief Read from the raw storage partition. Needs @ref APP_CAP_FLASH.
 *
 * Lock-serialized: the partition is a shared peripheral, so accesses are
 * serialized. The offset is relative to the window the application was
 * granted (see @ref app_platform_grant_flash), never the whole partition.
 *
 * @param off Offset within the application's window.
 * @param buf Receives the bytes.
 * @param len Number of bytes (bounded by the platform).
 * @retval 0 Success.
 * @retval -EPERM The application does not hold @ref APP_CAP_FLASH.
 * @retval -EACCES No window granted, or the range leaves it.
 */
__syscall int svc_flash_read(uint32_t off, uint8_t *buf, size_t len);

/**
 * @brief Write to the raw storage partition. Needs @ref APP_CAP_FLASH.
 *
 * Lock-serialized and bounded by the application's granted window.
 *
 * @param off Offset within the application's window.
 * @param buf Bytes to write.
 * @param len Number of bytes (bounded by the platform).
 * @retval >=0 Bytes written.
 * @retval -EPERM The application does not hold @ref APP_CAP_FLASH.
 * @retval -EACCES No window granted, or the range leaves it.
 */
__syscall int svc_flash_write(uint32_t off, const uint8_t *buf, size_t len);

/**
 * @brief Erase a span of the raw storage partition. Needs @ref APP_CAP_FLASH.
 *
 * Lock-serialized and bounded by the application's granted window.
 *
 * @param off Offset within the application's window.
 * @param len Length to erase.
 * @retval 0 Success.
 * @retval -EPERM The application does not hold @ref APP_CAP_FLASH.
 * @retval -EACCES No window granted, or the range leaves it.
 */
__syscall int svc_flash_erase(uint32_t off, size_t len);

/**
 * @brief Grant an application a window into the storage partition.
 *
 * The partition also holds the registry and the signed modules, so an
 * application addresses only the range granted here; offsets passed to the
 * flash services are relative to @p base. A length of zero revokes the grant.
 *
 * @param id Application id.
 * @param base Absolute offset of the window within the partition.
 * @param len Window length in bytes, or 0 for no access.
 */
void app_platform_grant_flash(int id, uint32_t base, uint32_t len);

/** Metadata for one announced sensor service. */
struct svc_sensor_info {
	char name[16];   /**< service name, for example "temperature" */
	char unit[10];   /**< unit of the value, for example "m.degC" */
	bool present;    /**< a device on this board backs the kind    */
};

/**
 * @brief Number of sensor kinds the platform announces.
 *        Needs @ref APP_CAP_SENSOR.
 *
 * @retval >=0 The catalog size.
 * @retval -EPERM The application does not hold @ref APP_CAP_SENSOR.
 */
__syscall int svc_sensor_count(void);

/**
 * @brief Describe one announced sensor. Needs @ref APP_CAP_SENSOR.
 *
 * An announced sensor is not necessarily present: the platform knows the kind
 * but this board may carry no device for it. Read the catalog first and act on
 * @c present rather than assuming.
 *
 * @param idx Index below @ref svc_sensor_count.
 * @param out Receives the metadata.
 *
 * @retval 0 Success.
 * @retval -EPERM The application does not hold @ref APP_CAP_SENSOR.
 * @retval -EINVAL @p idx is out of range.
 */
__syscall int svc_sensor_info(int idx, struct svc_sensor_info *out);

/**
 * @brief Read a sensor by catalog index. Needs @ref APP_CAP_SENSOR.
 *
 * The value is in milli-units of the kind's unit, so a temperature comes back
 * in millidegrees Celsius. Concurrent readers are serialized on the device but
 * do not block one another for long.
 *
 * @param idx Index below @ref svc_sensor_count.
 * @param milli_value Receives the reading.
 *
 * @retval 0 Success.
 * @retval -EPERM The application does not hold @ref APP_CAP_SENSOR.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The kind is announced but no device backs it here.
 * @retval -EBUSY The device stayed busy with another reader.
 */
__syscall int svc_sensor_read(int idx, int32_t *milli_value);

/**
 * @brief Read the SoC unique device id. Needs @ref APP_CAP_TIME.
 *
 * Grouped under the time/identity capability. Stateless.
 *
 * @param buf Receives the id bytes.
 * @param len Buffer size (bounded by the platform).
 * @retval >=0 the id length, -EPERM without the capability.
 */
__syscall int svc_device_id(uint8_t *buf, size_t len);

/**
 * @brief Read the reset cause bitmask. Needs @ref APP_CAP_TIME. Stateless.
 *
 * @param cause Receives the reset-cause flags.
 * @retval 0 on success, -EPERM without the capability.
 */
__syscall int svc_reset_cause(uint32_t *cause);

/**
 * @brief Arm the calling application's one-shot timer. Needs @ref APP_CAP_TIME.
 *
 * Per-app-keyed: each application has its own timer. A later @ref svc_timer_wait
 * blocks until it fires - a software timer reached through the platform rather
 * than the application owning a kernel timer object.
 *
 * @param ms Milliseconds until the timer fires.
 * @retval 0 on success, -EPERM without the capability.
 */
__syscall int svc_timer_start(uint32_t ms);

/**
 * @brief Wait for the calling application's timer to fire. Needs @ref APP_CAP_TIME.
 *
 * @param timeout_ms Maximum time to wait.
 * @retval 0 if the timer fired, -EPERM without the capability, -EAGAIN on
 *         timeout.
 */
__syscall int svc_timer_wait(uint32_t timeout_ms);

/** Timer slots an application may hold at once. */
#define APP_TIMER_SLOTS 4

/**
 * @brief Claim one of the application's timer slots. Needs @ref APP_CAP_TIME.
 *
 * A slot is independent of @ref svc_timer_start and of the other slots, so an
 * application can run several timers at once and tell which one fired. Slots
 * are released when the application is reclaimed, so a faulting application
 * leaks none.
 *
 * @retval >=0 the slot index, in [0, @ref APP_TIMER_SLOTS).
 * @retval -EPERM without @ref APP_CAP_TIME.
 * @retval -ENOSPC when the application already holds every slot.
 */
__syscall int svc_timer_create(void);

/**
 * @brief Arm a claimed slot. Needs @ref APP_CAP_TIME.
 *
 * Arming a running slot restarts it. A periodic slot re-arms itself at the same
 * interval until stopped, so one call covers a recurring tick.
 *
 * @param slot     Slot from @ref svc_timer_create.
 * @param ms       Milliseconds until it fires, and the repeat interval when
 *                 @p periodic is non-zero. Zero stops the slot.
 * @param periodic Non-zero to re-arm automatically.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_TIME.
 * @retval -EINVAL on an unclaimed or out-of-range slot.
 */
__syscall int svc_timer_arm(int slot, uint32_t ms, int periodic);

/**
 * @brief Wait for a claimed slot to fire. Needs @ref APP_CAP_TIME.
 *
 * A periodic slot that fired more than once while the application was busy
 * reports a single pending fire, so a slow reader coalesces rather than
 * accumulating a backlog.
 *
 * @param slot       Slot from @ref svc_timer_create.
 * @param timeout_ms Milliseconds to wait, negative to wait for ever.
 * @retval 0 if the slot fired.
 * @retval -EPERM without @ref APP_CAP_TIME.
 * @retval -EINVAL on an unclaimed or out-of-range slot.
 * @retval -EAGAIN if it did not fire before the timeout.
 */
__syscall int svc_timer_slot_wait(int slot, int32_t timeout_ms);

/**
 * @brief Stop a claimed slot without releasing it. Needs @ref APP_CAP_TIME.
 *
 * @param slot Slot from @ref svc_timer_create.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_TIME.
 * @retval -EINVAL on an unclaimed or out-of-range slot.
 */
__syscall int svc_timer_stop(int slot);

/**
 * @brief Milliseconds since a claimed slot was armed. Needs @ref APP_CAP_TIME.
 *
 * @param slot Slot from @ref svc_timer_create.
 * @retval >=0 elapsed milliseconds, 0 for a slot that is not running.
 * @retval -EPERM without @ref APP_CAP_TIME.
 * @retval -EINVAL on an unclaimed or out-of-range slot.
 */
__syscall int svc_timer_elapsed(int slot);

/**
 * @brief Release a claimed slot. Needs @ref APP_CAP_TIME.
 *
 * The slot is stopped and returned to the application's pool.
 *
 * @param slot Slot from @ref svc_timer_create.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_TIME.
 * @retval -EINVAL on an unclaimed or out-of-range slot.
 */
__syscall int svc_timer_free(int slot);

/**
 * @brief Read the wall-clock time in seconds. Needs @ref APP_CAP_TIME.
 *
 * The platform keeps a monotonic wall clock seeded by @ref svc_rtc_set; this
 * returns that seed advanced by elapsed time. Lock-serialized.
 *
 * @param epoch_sec Receives the seconds since the set epoch.
 * @retval 0 on success, -EPERM without the capability.
 */
__syscall int svc_rtc_now(uint32_t *epoch_sec);

/**
 * @brief Seed the wall clock. Needs @ref APP_CAP_TIME. Lock-serialized.
 *
 * @param epoch_sec Seconds value to set as the current time.
 * @retval 0 on success, -EPERM without the capability.
 */
__syscall int svc_rtc_set(uint32_t epoch_sec);

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
 * @brief Receive a message from the application's subscribed topic.
 *        Needs @ref APP_CAP_NET.
 *
 * Returns the next message that arrived on the topic passed as sub_topic to
 * @ref svc_mqtt_open, copied into @p payload and NUL-terminated. Per-app-keyed:
 * reads only this application's own session.
 *
 * @param payload Destination buffer.
 * @param cap Size of @p payload.
 * @param timeout_ms How long to wait for a message.
 * @retval >=0 the payload length, -EAGAIN on timeout, -EPERM without the
 *         capability, negative errno otherwise.
 */
__syscall int svc_mqtt_recv(char *payload, size_t cap, uint32_t timeout_ms);

/** Snapshot of the shared Wi-Fi link, filled by @ref svc_wifi_status. */
struct svc_wifi_status {
	uint8_t  connected;  /**< Non-zero when the link has an IPv4 address. */
	char     ssid[33];   /**< SSID the link is joined to (NUL-terminated). */
	char     ipv4[16];   /**< Dotted IPv4 address, empty when none. */
	int8_t   rssi;       /**< Cached signal strength in dBm, 0 if unknown. */
};

/**
 * @brief Read the shared Wi-Fi link's status. Needs @ref APP_CAP_NET.
 *
 * Fills @p out with the current connection state of the one shared link. Does
 * not change anything; safe to call from any networking application.
 *
 * @param out Destination status snapshot.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_wifi_status(struct svc_wifi_status *out);

/**
 * @brief Set the shared Wi-Fi credentials and reconnect. Needs @ref APP_CAP_NET.
 *
 * Replaces the shared link's credentials with @p ssid / @p psk, persists them so
 * they survive a reboot, and brings the one shared link down and back up on the
 * new network. Because the link is shared, this affects every networking
 * application. Returns once the new link is ready or has failed.
 *
 * @param ssid Network SSID.
 * @param psk Pre-shared key; pass an empty string for an open network.
 * @retval 0 when the new link is ready, -EPERM without the capability, negative
 *         errno on failure.
 */
__syscall int svc_wifi_connect(const char *ssid, const char *psk);

/**
 * @brief Bring the shared Wi-Fi link down. Needs @ref APP_CAP_NET.
 *
 * Disconnects the one shared link. Because the link is shared, this affects
 * every networking application.
 *
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_wifi_disconnect(void);

/**
 * @brief Resolve a hostname to an IPv4 address. Needs @ref APP_CAP_SOCKET.
 *
 * Turns @p host into a dotted IPv4 string through the platform's resolver. The
 * shared Wi-Fi link must be up (see @ref svc_net_ready).
 *
 * @param host Hostname or dotted address to resolve.
 * @param out_ip Destination for the dotted IPv4 string.
 * @param cap Size of @p out_ip (16 is enough for IPv4).
 * @retval 0 on success, -ENOSPC if the address does not fit in @p cap, -EPERM
 *         without the capability, negative errno otherwise.
 */
__syscall int svc_resolve(const char *host, char *out_ip, size_t cap);

/**
 * @brief Open the calling application's UDP socket to a peer.
 *        Needs @ref APP_CAP_SOCKET.
 *
 * Per-app-keyed: each application gets one platform-owned UDP socket bound to a
 * peer @p host : @p port. The application never holds the socket. The shared
 * Wi-Fi link must be up first (see @ref svc_net_ready).
 *
 * @param host Peer hostname or dotted IPv4 address.
 * @param port Peer UDP port.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_udp_open(const char *host, uint16_t port);

/**
 * @brief Send a datagram on the application's UDP socket. Needs @ref APP_CAP_SOCKET.
 *
 * @param buf Payload.
 * @param len Payload length, at most CONFIG_APP_PLATFORM_NET_DGRAM_MAX.
 * @retval >=0 bytes queued, -EMSGSIZE if @p len is too long, -EPERM without
 *         the capability, negative errno otherwise.
 */
__syscall int svc_udp_send(const void *buf, size_t len);

/**
 * @brief Receive a datagram from the application's UDP socket.
 *        Needs @ref APP_CAP_SOCKET.
 *
 * A datagram longer than @p cap or CONFIG_APP_PLATFORM_NET_DGRAM_MAX is
 * truncated.
 *
 * @param buf Destination buffer.
 * @param cap Size of @p buf.
 * @param timeout_ms How long to wait for a datagram.
 * @retval >=0 the datagram length, -EAGAIN on timeout, -EPERM without the
 *         capability, negative errno otherwise.
 */
__syscall int svc_udp_recv(void *buf, size_t cap, uint32_t timeout_ms);

/**
 * @brief Receive a datagram and learn who sent it. Needs @ref APP_CAP_SOCKET.
 *
 * Like @ref svc_udp_recv, but also reports the sender's address and port. Pass
 * NULL for @p src_ip or @p src_port to skip either.
 *
 * @param buf Destination buffer.
 * @param cap Size of @p buf.
 * @param timeout_ms How long to wait for a datagram.
 * @param src_ip Destination for the sender's dotted IPv4 string, or NULL.
 * @param ip_cap Size of @p src_ip (16 is enough for IPv4).
 * @param src_port Destination for the sender's port, or NULL.
 * @retval >=0 the datagram length, -EAGAIN on timeout, -EPERM without the
 *         capability, negative errno otherwise.
 */
__syscall int svc_udp_recv_from(void *buf, size_t cap, uint32_t timeout_ms,
				char *src_ip, size_t ip_cap, uint16_t *src_port);

/**
 * @brief Close the application's UDP socket. Needs @ref APP_CAP_SOCKET.
 *
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_udp_close(void);

/**
 * @brief Open the calling application's TCP stream to a server.
 *        Needs @ref APP_CAP_SOCKET.
 *
 * Per-app-keyed: each application gets one platform-owned TCP socket connected
 * to @p host : @p port. The application never holds the socket. The shared
 * Wi-Fi link must be up first (see @ref svc_net_ready).
 *
 * @param host Server hostname or dotted IPv4 address.
 * @param port Server TCP port.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_tcp_connect(const char *host, uint16_t port);

/**
 * @brief Send bytes on the application's TCP stream. Needs @ref APP_CAP_SOCKET.
 *
 * @param buf Payload.
 * @param len Payload length.
 * @retval >=0 bytes sent, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_tcp_send(const void *buf, size_t len);

/**
 * @brief Receive bytes from the application's TCP stream.
 *        Needs @ref APP_CAP_SOCKET.
 *
 * @param buf Destination buffer.
 * @param cap Size of @p buf.
 * @param timeout_ms How long to wait for data.
 * @retval >0 bytes read, 0 on a closed stream, -EAGAIN on timeout, -EPERM
 *         without the capability, negative errno otherwise.
 */
__syscall int svc_tcp_recv(void *buf, size_t cap, uint32_t timeout_ms);

/**
 * @brief Close the application's TCP stream. Needs @ref APP_CAP_SOCKET.
 *
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_tcp_close(void);

/**
 * @brief Number of GPIO lines the platform exposes. Needs @ref APP_CAP_GPIO.
 *
 * The lines come from the board's @c leds devicetree node.
 *
 * @retval >=0 The number of lines, which may be zero.
 * @retval -EPERM The application does not hold @ref APP_CAP_GPIO.
 */
__syscall int svc_gpio_count(void);

/**
 * @brief Drive a GPIO line. Needs @ref APP_CAP_GPIO and a per-line grant.
 *
 * Per-line grant: holding @ref APP_CAP_GPIO is not enough; the application is
 * granted a specific set of lines (see @ref app_platform_grant_gpio) and a call
 * to an ungranted line is refused. A line the application last drove high is
 * driven low when the application is reclaimed.
 *
 * @param line Line index in [0, @ref svc_gpio_count).
 * @param on   Non-zero to drive high.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_GPIO.
 * @retval -EACCES with the capability but without a grant for this line.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_gpio_set(int line, int on);

/**
 * @brief Resolve a GPIO line's devicetree name to its index. Needs @ref
 *        APP_CAP_GPIO. Resolve once and pass the index to @ref svc_gpio_set.
 *
 * @param name Line devicetree label (or node name when it has no label).
 * @retval >=0 the line index, -ENOENT if no line has that name.
 */
__syscall int svc_gpio_find(const char *name);

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

/**
 * @brief Kinds of event delivered through @ref svc_event_wait.
 */
enum app_event_kind {
	APP_EVENT_NONE = 0,   /**< no event (timeout) */
	APP_EVENT_BUTTON,     /**< the board button was pressed */
	APP_EVENT_GPIO,       /**< a granted input line changed */
	APP_EVENT_GATT_WRITE, /**< a peer wrote a served characteristic */
	APP_EVENT_BLE_CONNECTED,    /**< a peer connected to the served app */
	APP_EVENT_BLE_DISCONNECTED, /**< the connected peer went away */
	APP_EVENT_STOP,       /**< the platform asked the application to exit */
};

/**
 * @brief One input event delivered to an application.
 */
struct app_event {
	uint8_t  kind;   /**< one of @ref app_event_kind */
	uint8_t  line;   /**< GPIO line index, or served characteristic index */
	uint8_t  value;  /**< line level, or pressed state for a key */
	uint16_t code;   /**< key code, for @ref APP_EVENT_BUTTON */
	uint32_t seq;    /**< per-application counter, gaps mean dropped events */
};

/**
 * @brief Wait for the next input event. Needs @ref APP_CAP_INPUT.
 *
 * Each application has its own queue, so one application waiting does not
 * consume another's events, and a single wait covers every source the
 * application is subscribed to. When the queue overflows the oldest events are
 * dropped and @c seq skips, so a reader can tell it fell behind.
 *
 * Pass 0 for @p timeout_ms to poll: the call reports -EAGAIN at once when the
 * queue is empty, so an application driving something else can fold input into
 * its own loop instead of parking a thread on it.
 *
 * @param[out] evt       Receives the event.
 * @param timeout_ms     Milliseconds to wait, 0 to poll, negative to wait for
 *                       ever.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_INPUT.
 * @retval -EAGAIN if no event arrived before the timeout.
 */
__syscall int svc_event_wait(struct app_event *evt, int32_t timeout_ms);

/**
 * @brief Subscribe to level changes on a granted GPIO line. Needs
 *        @ref APP_CAP_GPIO and a per-line grant.
 *
 * The line is configured as an input and its edges are reported through
 * @ref svc_event_wait as @ref APP_EVENT_GPIO. An ungranted line is refused, so
 * an application cannot watch a line it may not drive.
 *
 * @param line Line index in [0, @ref svc_gpio_count).
 * @param on   Non-zero to subscribe, zero to stop.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_GPIO.
 * @retval -EACCES with the capability but without a grant for this line.
 */
__syscall int svc_gpio_watch(int line, int on);

/**
 * @brief Send a message to another application's mailbox. Needs @ref APP_CAP_IPC.
 *
 * Per-app-keyed: the bytes are copied into the destination application's own
 * mailbox, so applications exchange messages without sharing memory. Only the
 * copied bytes cross the boundary.
 *
 * @param dest_id Destination application group id.
 * @param buf     Message bytes.
 * @param len     Message length (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability, -EAGAIN if the mailbox is
 *         full, negative errno otherwise.
 */
__syscall int svc_ipc_send(int dest_id, const void *buf, size_t len);

/**
 * @brief Receive a message from the calling application's mailbox.
 * Needs @ref APP_CAP_IPC.
 *
 * Per-app-keyed: reads only the calling application's own mailbox.
 *
 * @param buf        Receives the message bytes.
 * @param len        Buffer size (bounded by the platform).
 * @param timeout_ms Milliseconds to wait, or 0 to poll.
 * @retval >=0 the received length, -EPERM without the capability, -EAGAIN on
 *         timeout.
 */
__syscall int svc_ipc_recv(void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief Register the calling application under a name. Needs @ref APP_CAP_IPC.
 *
 * Lets other applications address this one by name (see @ref svc_ipc_send_to)
 * instead of by numeric group id. The name is copied into the platform.
 *
 * @param name Unique application name.
 * @retval 0 on success, -EPERM without the capability, -EINVAL on a bad name.
 */
__syscall int svc_ipc_register(const char *name);

/**
 * @brief Send a message to a named application. Needs @ref APP_CAP_IPC.
 *
 * Resolves @p name to its mailbox and copies the bytes in, the same isolation-
 * safe delivery as @ref svc_ipc_send but addressed by name.
 *
 * @param name Destination application name (see @ref svc_ipc_register).
 * @param buf  Message bytes.
 * @param len  Message length (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability, -ENOENT for an unknown
 *         name, -EAGAIN if the mailbox is full.
 */
__syscall int svc_ipc_send_to(const char *name, const void *buf, size_t len);

/**
 * @brief Subscribe the calling application to a topic. Needs @ref APP_CAP_IPC.
 *
 * A message published to the topic is copied into this application's mailbox and
 * read with @ref svc_ipc_recv, like any other message. Each application has a
 * single topic subscription.
 *
 * @param topic Topic name.
 * @retval 0 on success, -EPERM without the capability.
 */
__syscall int svc_topic_subscribe(const char *topic);

/**
 * @brief Publish a message to a topic. Needs @ref APP_CAP_IPC.
 *
 * Delivers a copy of the message into the mailbox of every application
 * subscribed to @p topic - one-to-many broadcast without shared memory, the
 * isolation-safe equivalent of a publish/subscribe channel.
 *
 * @param topic Topic name.
 * @param buf   Message bytes.
 * @param len   Message length (bounded by the platform).
 * @retval >=0 the number of subscribers delivered to, -EPERM without the
 *         capability.
 */
__syscall int svc_topic_publish(const char *topic, const void *buf, size_t len);

/** A user-mode-safe snapshot of one application group. */
struct svc_app_info {
	char     name[16];
	uint8_t  active;
	uint8_t  monitored;
	uint8_t  faulted;
	uint32_t stack_size;
	uint32_t stack_used;
	uint64_t cpu_cycles;
	uint64_t cpu_total;
};

/**
 * @brief Number of application group slots. Needs @ref APP_CAP_INSPECT.
 *
 * @retval >=0 The number of slots.
 * @retval -EPERM The application does not hold @ref APP_CAP_INSPECT.
 */
__syscall int svc_app_count(void);

/**
 * @brief Read a snapshot of an application group. Needs @ref APP_CAP_INSPECT.
 *
 * Lets a granted application observe the others' stack and CPU use. The name is
 * copied into @p out, so the caller never holds a kernel pointer. An application
 * without the capability is refused, so one cannot observe the others uninvited.
 *
 * @param id  Group id.
 * @param out Receives the snapshot.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_app_info(int id, struct svc_app_info *out);

/**
 * @brief Stop a running application group. Needs @ref APP_CAP_CONTROL.
 *
 * The group keeps its slot and its loaded module, so @ref svc_app_start can run
 * it again. An application may not stop itself: aborting the caller inside its
 * own system call would tear the thread down mid-call, so the request is
 * refused instead.
 *
 * @param id Group id.
 * @retval 0 on success.
 * @retval -EPERM The application does not hold @ref APP_CAP_CONTROL.
 * @retval -EINVAL @p id is the caller, or is not a running group.
 */
__syscall int svc_app_stop(int id);

/**
 * @brief Start a stopped application group. Needs @ref APP_CAP_CONTROL.
 *
 * Re-creates the group's user-mode thread in its existing memory domain, with
 * the capabilities it was loaded with. A controlling application therefore
 * cannot widen another group's grants, only run or halt it.
 *
 * @param id Group id.
 * @retval 0 on success.
 * @retval -EPERM The application does not hold @ref APP_CAP_CONTROL.
 * @retval -EINVAL @p id is the caller, or is not a loaded group.
 * @retval -EALREADY The group is already running.
 * @retval -ENOTSUP The group cannot be started again.
 * @retval -EBUSY A previous stop of this group is still being cleaned up.
 */
__syscall int svc_app_start(int id);

/**
 * @brief Stop an application and start it again. Needs @ref APP_CAP_CONTROL.
 *
 * One call so a supervising application does not have to sequence a stop and a
 * start itself, and so the platform can refuse the pair together. An
 * application that is already stopped is simply started. A caller may not
 * restart itself.
 *
 * @param id Application group id, not the caller's own.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_CONTROL.
 * @retval -EINVAL on a bad id or the caller's own id.
 * @retval -ENOTSUP when loadable applications are not built in.
 */
__syscall int svc_app_restart(int id);

/**
 * @brief Number of hardware counter instances the board announces. Needs
 *        @ref APP_CAP_COUNTER_DEV.
 *
 * Counting is not gated per instance, so an application can size its own table
 * before asking for one it was not granted.
 *
 * @retval >=0 The number of instances.
 * @retval -EPERM The application does not hold @ref APP_CAP_COUNTER_DEV.
 */
__syscall int svc_counter_count(void);

/**
 * @brief Start a granted hardware counter. Needs @ref APP_CAP_COUNTER_DEV and a
 *        per-instance grant (@ref app_platform_grant_counter).
 *
 * A counter the application last started is stopped when the application is
 * reclaimed.
 *
 * @param idx Counter index.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_COUNTER_DEV.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_counter_start(int idx);

/**
 * @brief Stop a granted hardware counter. Needs @ref APP_CAP_COUNTER_DEV and a
 *        per-instance grant.
 *
 * @param idx Counter index.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_COUNTER_DEV.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_counter_stop(int idx);

/**
 * @brief Read the current tick value of a granted counter. Needs @ref
 *        APP_CAP_COUNTER_DEV and a per-instance grant.
 *
 * @param idx   Counter index.
 * @param ticks Receives the tick value.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_COUNTER_DEV.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 */
__syscall int svc_counter_read(int idx, uint32_t *ticks);

/**
 * @brief Read the tick frequency of a granted counter. Needs @ref
 *        APP_CAP_COUNTER_DEV and a per-instance grant.
 *
 * Ticks are meaningless without it, so an application converts an interval to
 * time with this rather than assuming a rate.
 *
 * @param idx Counter index.
 * @param hz  Receives the frequency in hertz.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_COUNTER_DEV.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 */
__syscall int svc_counter_frequency(int idx, uint32_t *hz);

/** @brief Grant an application a set of hardware counter instances. */
void app_platform_grant_counter(int id, uint32_t idx_mask);

/**
 * @brief Number of analog output channels. Needs @ref APP_CAP_DAC.
 *
 * @retval >=0 The number of channels.
 * @retval -EPERM The application does not hold @ref APP_CAP_DAC.
 */
__syscall int svc_dac_count(void);

/**
 * @brief Drive a granted analog output channel. Needs @ref APP_CAP_DAC and a
 *        per-channel grant (@ref app_platform_grant_dac).
 *
 * A channel the application last set to a non-zero code is written 0 when the
 * application is reclaimed.
 *
 * @param ch    Channel index.
 * @param value Raw code, bounded by the configured resolution.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_DAC.
 * @retval -EACCES with the capability but without a grant for this channel.
 * @retval -EINVAL @p ch is out of range.
 * @retval -ENODEV The channel is not ready.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_dac_write(int ch, uint32_t value);

/** @brief Grant an application a set of analog output channels. */
void app_platform_grant_dac(int id, uint32_t ch_mask);

/**
 * @brief Size of the application's own EEPROM window. Needs @ref
 *        APP_CAP_EEPROM.
 *
 * @retval >=0 The window size in bytes, zero when none was granted.
 * @retval -EPERM The application does not hold @ref APP_CAP_EEPROM.
 */
__syscall int svc_eeprom_size(void);

/**
 * @brief Read from the application's EEPROM window. Needs @ref APP_CAP_EEPROM
 *        and a window (@ref app_platform_grant_eeprom).
 *
 * @p off is relative to the window, so an application cannot name a byte
 * outside it, including another application's.
 *
 * @param off Offset within the window.
 * @param buf Receives the bytes.
 * @param len Number of bytes (bounded by the platform).
 * @retval >=0 The number of bytes read.
 * @retval -EPERM without @ref APP_CAP_EEPROM.
 * @retval -EACCES no window was granted.
 * @retval -EINVAL the range leaves the window, or @p len exceeds the limit.
 * @retval -ENODEV The device is not ready.
 */
__syscall int svc_eeprom_read(uint32_t off, void *buf, size_t len);

/**
 * @brief Write to the application's EEPROM window. Needs @ref APP_CAP_EEPROM
 *        and a window.
 *
 * @param off Offset within the window.
 * @param buf Bytes to write.
 * @param len Number of bytes (bounded by the platform).
 * @retval >=0 The number of bytes written.
 * @retval -EPERM without @ref APP_CAP_EEPROM.
 * @retval -EACCES no window was granted.
 * @retval -EINVAL the range leaves the window, or @p len exceeds the limit.
 * @retval -ENODEV The device is not ready.
 */
__syscall int svc_eeprom_write(uint32_t off, const void *buf, size_t len);

/** @brief Grant an application a byte window of the EEPROM. */
void app_platform_grant_eeprom(int id, size_t base, size_t len);

/** A user-mode-safe wall-clock time. */
struct svc_rtc_time {
	uint16_t year;    /**< full year, e.g. 2026 */
	uint8_t  month;   /**< month [1, 12] */
	uint8_t  day;     /**< day of the month [1, 31] */
	uint8_t  hour;    /**< hour [0, 23] */
	uint8_t  minute;  /**< minute [0, 59] */
	uint8_t  second;  /**< second [0, 59] */
};

/**
 * @brief Read the real-time clock device. Needs @ref APP_CAP_RTC.
 *
 * Distinct from @ref svc_rtc_now, which reads the platform's software wall
 * clock: this reaches the board's RTC hardware.
 *
 * @param out Receives the time.
 * @retval 0 on success.
 * @retval -EPERM The application does not hold @ref APP_CAP_RTC.
 * @retval -ENODEV The clock is not ready.
 */
__syscall int svc_rtcdev_get(struct svc_rtc_time *out);

/**
 * @brief Set the real-time clock device. Needs @ref APP_CAP_RTC_SET.
 *
 * Separated from reading because the clock is device-wide: setting it changes
 * what every other application sees.
 *
 * @param in The time to set.
 * @retval 0 on success.
 * @retval -EPERM The application does not hold @ref APP_CAP_RTC_SET.
 * @retval -EINVAL A field is out of range.
 * @retval -ENODEV The clock is not ready.
 */
__syscall int svc_rtcdev_set(const struct svc_rtc_time *in);

/**
 * @brief Reset the 1-Wire bus and report whether a device answered. Needs
 *        @ref APP_CAP_W1.
 *
 * @retval >=0 Non-zero when at least one device responded.
 * @retval -EPERM The application does not hold @ref APP_CAP_W1.
 * @retval -ENODEV The bus is not ready.
 */
__syscall int svc_w1_reset(void);

/**
 * @brief Read one byte from the 1-Wire bus. Needs @ref APP_CAP_W1.
 *
 * @retval >=0 The byte read.
 * @retval -EPERM The application does not hold @ref APP_CAP_W1.
 * @retval -ENODEV The bus is not ready.
 */
__syscall int svc_w1_read_byte(void);

/**
 * @brief Write one byte to the 1-Wire bus. Needs @ref APP_CAP_W1.
 *
 * @param byte The byte to write.
 * @retval 0 on success.
 * @retval -EPERM The application does not hold @ref APP_CAP_W1.
 * @retval -ENODEV The bus is not ready.
 */
__syscall int svc_w1_write_byte(uint8_t byte);

/**
 * @brief Whether external power is present. Needs @ref APP_CAP_POWER.
 *
 * @retval >=0 Non-zero when the charger reports itself online.
 * @retval -EPERM The application does not hold @ref APP_CAP_POWER.
 * @retval -ENODEV The charger is not ready.
 */
__syscall int svc_charger_online(void);

/**
 * @brief Battery charge in percent. Needs @ref APP_CAP_POWER.
 *
 * @retval >=0 The relative state of charge, 0 to 100.
 * @retval -EPERM The application does not hold @ref APP_CAP_POWER.
 * @retval -ENODEV The fuel gauge is not ready.
 */
__syscall int svc_battery_percent(void);

/**
 * @brief Number of regulator instances. Needs @ref APP_CAP_REGULATOR.
 *
 * @retval >=0 The number of instances.
 * @retval -EPERM The application does not hold @ref APP_CAP_REGULATOR.
 */
__syscall int svc_regulator_count(void);

/**
 * @brief Enable or disable a granted regulator. Needs @ref APP_CAP_REGULATOR
 *        and a per-instance grant (@ref app_platform_grant_regulator).
 *
 * Enables are counted per application. A disable releases one of the calling
 * application's own enables, and the enables it still holds are released when
 * it is reclaimed.
 *
 * @param idx Regulator index.
 * @param on  Non-zero to enable, zero to disable.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_REGULATOR.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 * @retval -EALREADY Disable while the application holds no enable.
 * @retval -EOVERFLOW Enable while the application's enable count is saturated.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_regulator_enable(int idx, int on);

/** @brief Grant an application a set of regulator instances. */
void app_platform_grant_regulator(int id, uint32_t idx_mask);

/**
 * @brief Number of stepper motor instances. Needs @ref APP_CAP_STEPPER.
 *
 * @retval >=0 The number of instances.
 * @retval -EPERM The application does not hold @ref APP_CAP_STEPPER.
 */
__syscall int svc_stepper_count(void);

/**
 * @brief Stop a granted stepper's movement. Needs @ref APP_CAP_STEPPER and a
 *        per-instance grant (@ref app_platform_grant_stepper).
 *
 * @param idx Stepper index.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_STEPPER.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_stepper_stop(int idx);

/**
 * @brief Move a granted stepper by a relative number of micro-steps. Needs
 *        @ref APP_CAP_STEPPER and a per-instance grant.
 *
 * The move is not awaited: completion is reported through a driver callback,
 * which cannot cross into user mode, so an application polls
 * @ref svc_stepper_position instead. A stepper the application last moved is
 * stopped when the application is reclaimed.
 *
 * @param idx         Stepper index.
 * @param micro_steps Signed micro-step count.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_STEPPER.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_stepper_move_by(int idx, int32_t micro_steps);

/**
 * @brief Read a granted stepper's current position. Needs @ref APP_CAP_STEPPER
 *        and a per-instance grant.
 *
 * @param idx Stepper index.
 * @param pos Receives the position in micro-steps.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_STEPPER.
 * @retval -EACCES with the capability but without a grant for this instance.
 * @retval -EINVAL @p idx is out of range.
 * @retval -ENODEV The instance is not ready.
 */
__syscall int svc_stepper_position(int idx, int32_t *pos);

/** @brief Grant an application a set of stepper instances. */
void app_platform_grant_stepper(int id, uint32_t idx_mask);

/**
 * @brief Open a file under the application's private root. Needs @ref
 *        APP_CAP_FS.
 *
 * @p path is relative to the application's own root, which is named after the
 * application, so its files persist across restarts and two applications that
 * open the same name get two different files. A path that is absolute or holds
 * a ".." component is refused: an application cannot name a file outside its
 * root, including another application's.
 *
 * @param path   File path, relative to the application's root.
 * @param create Create the file when it does not exist.
 * @retval >=0 An open handle, valid only for the application that opened it.
 * @retval -EPERM The application does not hold @ref APP_CAP_FS.
 * @retval -EACCES The path tries to leave the application's root.
 * @retval -EINVAL The path is empty, or the application name is not a plain
 *         directory name (letters, digits, '_', '-', '.', not starting with '.').
 * @retval -ENAMETOOLONG The resolved path does not fit.
 * @retval -EMFILE No free handle is left.
 * @retval -ENODEV The filesystem is not mounted.
 */
__syscall int svc_file_open(const char *path, int create);

/**
 * @brief Write to an open file. Needs @ref APP_CAP_FS.
 *
 * @param handle Handle from @ref svc_file_open.
 * @param data   Bytes to write.
 * @param len    Number of bytes (bounded by the platform).
 * @retval >=0 The number of bytes written.
 * @retval -EPERM The application does not hold @ref APP_CAP_FS.
 * @retval -EACCES The handle belongs to another application.
 * @retval -EBADF The handle is not open.
 * @retval -EINVAL @p len exceeds the platform's transfer limit.
 */
__syscall int svc_file_write(int handle, const void *data, size_t len);

/**
 * @brief Read from an open file. Needs @ref APP_CAP_FS.
 *
 * @param handle Handle from @ref svc_file_open.
 * @param buf    Receives the bytes read.
 * @param len    Maximum number of bytes (bounded by the platform).
 * @retval >=0 The number of bytes read.
 * @retval -EPERM The application does not hold @ref APP_CAP_FS.
 * @retval -EACCES The handle belongs to another application.
 * @retval -EBADF The handle is not open.
 * @retval -EINVAL @p len exceeds the platform's transfer limit.
 */
__syscall int svc_file_read(int handle, void *buf, size_t len);

/**
 * @brief Move the read/write position of an open file. Needs @ref APP_CAP_FS.
 *
 * @param handle Handle from @ref svc_file_open.
 * @param off    Offset in bytes.
 * @param whence One of the filesystem's FS_SEEK_* values.
 * @retval 0 on success.
 * @retval -EPERM The application does not hold @ref APP_CAP_FS.
 * @retval -EACCES The handle belongs to another application.
 * @retval -EBADF The handle is not open.
 */
__syscall int svc_file_seek(int handle, int32_t off, int whence);

/**
 * @brief Close an open file. Needs @ref APP_CAP_FS.
 *
 * Files an application leaves open are closed for it when it is reclaimed.
 *
 * @param handle Handle from @ref svc_file_open.
 * @retval 0 on success.
 * @retval -EPERM The application does not hold @ref APP_CAP_FS.
 * @retval -EACCES The handle belongs to another application.
 * @retval -EBADF The handle is not open.
 */
__syscall int svc_file_close(int handle);

/**
 * @brief Set a PWM channel duty cycle in per-mille. Needs @ref APP_CAP_PWM and a
 *        per-channel grant (@ref app_platform_grant_pwm).
 *
 * A channel the application last set to a non-zero duty is set to zero duty
 * when the application is reclaimed.
 *
 * @param ch       Channel index.
 * @param permille Duty cycle in tenths of a percent (0..1000).
 * @retval 0 on success.
 * @retval -EINVAL Invalid channel or a duty above 1000.
 * @retval -EPERM without @ref APP_CAP_PWM.
 * @retval -EACCES with the capability but without a grant for this channel.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_pwm_set(int ch, uint32_t permille);

/**
 * @brief Resolve a PWM channel's devicetree name to its index. Needs @ref
 *        APP_CAP_PWM. Resolve once and pass the index to @ref svc_pwm_set.
 *
 * @param name PWM channel devicetree label (or node name when it has none).
 * @retval >=0 the channel index, -ENOENT if no channel has that name.
 */
__syscall int svc_pwm_find(const char *name);

/** @brief Grant an application a set of PWM channels. */
void app_platform_grant_pwm(int id, uint32_t ch_mask);

/**
 * @brief Read an ADC channel. Needs @ref APP_CAP_ADC and a per-channel grant
 *        (@ref app_platform_grant_adc).
 *
 * @param ch Channel index.
 * @retval >=0 the raw sample.
 * @retval -EPERM without @ref APP_CAP_ADC.
 * @retval -EACCES with the capability but without a grant for this channel.
 */
__syscall int svc_adc_read(int ch);

/** @brief Grant an application a set of ADC channels. */
void app_platform_grant_adc(int id, uint32_t ch_mask);

/**
 * @brief Drive a granted status LED. Needs @ref APP_CAP_LED and a per-pin grant
 *        (@ref app_platform_grant_led).
 *
 * A LED the application last lit is cleared when the application is reclaimed.
 *
 * @param pin LED index in the range [0, @ref svc_led_count).
 * @param on Non-zero to light the LED, zero to clear it.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_LED.
 * @retval -EACCES with the capability but without a grant for this pin.
 * @retval -ENODEV if the board has no such LED.
 * @retval -EBUSY The service stayed locked by another caller.
 */
__syscall int svc_led_set(int pin, int on);

/**
 * @brief Number of status LEDs the platform exposes.
 *
 * Topology, so it is deliberately ungated: an application may ask how many
 * LEDs exist without holding @ref APP_CAP_LED.
 *
 * @return The number of LEDs, zero when the board exposes none.
 */
__syscall int svc_led_count(void);

/**
 * @brief Resolve a LED's devicetree name to its index. Resolve once at startup
 *        and pass the index to @ref svc_led_set, rather than hard-coding a
 *        position that shifts if LEDs are added.
 *
 * @param name LED devicetree label (or node name when it has no label).
 * @retval >=0 the LED index, -ENOENT if no LED has that name.
 */
__syscall int svc_led_find(const char *name);

/** @brief Grant an application a set of LED pins (bit N = pin N). */
void app_platform_grant_led(int id, uint32_t pin_mask);

/**
 * @brief Set one granted pixel of the addressable RGB LED strip and refresh the
 *        strip. Needs @ref APP_CAP_RGB and a per-pixel grant
 *        (@ref app_platform_grant_rgb).
 *
 * A pixel the application last set to a color other than black is turned off
 * when the application is reclaimed.
 *
 * @param pixel Pixel index in the range [0, @ref svc_rgb_count).
 * @param r Red intensity.
 * @param g Green intensity.
 * @param b Blue intensity.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_RGB.
 * @retval -EACCES with the capability but without a grant for this pixel.
 * @retval -ENODEV The LED strip is not ready.
 * @retval -EBUSY The service stayed locked by another caller.
 * @retval -errno Other negative errno code from the LED strip driver.
 */
__syscall int svc_rgb_set(int pixel, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Number of addressable RGB LED pixels the platform exposes.
 *
 * Topology, so it is deliberately ungated: an application may ask how many
 * pixels exist without holding @ref APP_CAP_RGB.
 *
 * @return The number of pixels.
 */
__syscall int svc_rgb_count(void);

/** @brief Grant an application a set of RGB LED pixels (bit N = pixel N). */
void app_platform_grant_rgb(int id, uint32_t pixel_mask);

/** A rectangle on the display, in panel coordinates. */
struct app_display_rect {
	uint16_t x;      /**< left column */
	uint16_t y;      /**< top row */
	uint16_t width;  /**< width in pixels */
	uint16_t height; /**< height in pixels */
};

/** What an application sees of the display: its viewport and the pixel layout. */
struct svc_display_info {
	uint16_t width;          /**< viewport width in pixels */
	uint16_t height;         /**< viewport height in pixels */
	uint32_t pixel_format;   /**< PIXEL_FORMAT_* value of the panel */
	uint8_t  bytes_per_pixel; /**< bytes per pixel in a write buffer */
};

/**
 * @brief Describe the calling application's viewport. Needs @ref APP_CAP_DISPLAY
 *        and a viewport grant (@ref app_platform_grant_display).
 *
 * @param out Filled with the viewport size and the panel pixel format.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_DISPLAY.
 * @retval -EACCES with the capability but without a viewport grant.
 * @retval -ENODEV The display is not ready.
 * @retval -ENOTSUP The panel uses a format smaller than one byte per pixel.
 */
__syscall int svc_display_info(struct svc_display_info *out);

/**
 * @brief Wait for the next display refresh. Needs @ref APP_CAP_DISPLAY and a
 *        viewport grant.
 *
 * Returns when the panel has finished scanning out a frame. A small update
 * drawn right after it stays ahead of the next scanout and shows without
 * tearing, and a loop that waits here runs at the panel refresh rate.
 *
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_DISPLAY.
 * @retval -EACCES with the capability but without a viewport grant.
 * @retval -ENODEV The display is not ready.
 * @retval -ENOTSUP The panel does not report its refresh.
 * @retval -EAGAIN No refresh was reported within 100 ms.
 */
__syscall int svc_display_vsync(void);

/**
 * @brief Draw a rectangle of pixels inside the calling application's viewport.
 *        Needs @ref APP_CAP_DISPLAY and a viewport grant.
 *
 * Coordinates are relative to the viewport, and the rectangle must lie fully
 * inside it: nothing is clipped. Rows are tightly packed in the panel format.
 * The viewport is filled with @kconfig{CONFIG_APP_PLATFORM_DISPLAY_CLEAR_COLOR}
 * when the application is reclaimed.
 *
 * @param x Left column in the viewport.
 * @param y Top row in the viewport.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @param buf Pixel data.
 * @param len Size of @p buf, which must be @p w * @p h * bytes per pixel.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_DISPLAY.
 * @retval -EACCES with the capability but without a viewport grant.
 * @retval -ERANGE The rectangle is empty or not fully inside the viewport.
 * @retval -EINVAL @p len does not match the rectangle.
 * @retval -ENODEV The display is not ready.
 * @retval -ENOTSUP The panel uses a format smaller than one byte per pixel.
 * @retval -EBUSY The display stayed locked by another caller.
 * @retval -EIO The display driver failed.
 */
__syscall int svc_display_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *buf,
				size_t len);

/**
 * @brief Fill a rectangle inside the calling application's viewport with one
 *        color. Needs @ref APP_CAP_DISPLAY and a viewport grant.
 *
 * Each pixel is the low bytes-per-pixel bytes of @p color, least significant
 * byte first. Bounds follow @ref svc_display_write.
 *
 * @param x Left column in the viewport.
 * @param y Top row in the viewport.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @param color Pixel value in the panel format.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_DISPLAY.
 * @retval -EACCES with the capability but without a viewport grant.
 * @retval -ERANGE The rectangle is empty or not fully inside the viewport.
 * @retval -ENODEV The display is not ready.
 * @retval -ENOTSUP The panel uses a format smaller than one byte per pixel.
 * @retval -EBUSY The display stayed locked by another caller.
 * @retval -EIO The display driver failed.
 */
__syscall int svc_display_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color);

/**
 * @brief Grant an application a viewport on the display.
 *
 * Viewports may overlap; a reclaim then also clears the overlapping pixels of
 * the other application.
 *
 * @param id Application slot.
 * @param vp Viewport in panel coordinates, or NULL or an empty rectangle to
 *           revoke the grant.
 * @retval 0 on success.
 * @retval -EINVAL @p id is not a slot or @p vp is not inside the panel.
 * @retval -ENODEV The display is not ready.
 */
int app_platform_grant_display(int id, const struct app_display_rect *vp);

/**
 * @brief Take the display lock from supervisor code.
 *
 * Panel drivers are not safe against concurrent calls, so supervisor code that
 * draws on the same panel takes this lock around its display calls, which then
 * never interleave with an application's writes.
 *
 * @param timeout How long to wait for the lock.
 * @retval 0 on success.
 * @retval -EBUSY The lock was not obtained within @p timeout.
 */
int app_platform_display_lock(k_timeout_t timeout);

/** @brief Release the display lock taken by @ref app_platform_display_lock. */
void app_platform_display_unlock(void);

/**
 * @brief Wait for the next button press. Needs @ref APP_CAP_INPUT.
 *
 * Blocks until the platform's button is pressed, then returns the running press
 * count. The platform owns the button and counts edges in supervisor mode.
 *
 * Pass 0 to poll: the call returns -EAGAIN at once when no press is waiting, so
 * an application that must keep working can check for input without blocking.
 * @ref svc_event_wait polls the same way and covers every input source at once.
 *
 * @param timeout_ms Milliseconds to wait, 0 to poll, or negative to wait
 *                   forever.
 * @retval >0 the running press count, -EAGAIN on timeout, -EPERM without the
 *         capability, -ENODEV if absent.
 */
__syscall int svc_button_wait(int32_t timeout_ms);

/**
 * @brief Store the calling application's checkpoint value.
 *
 * The value is kept by the platform outside the application's memory domain, so
 * it survives the application crashing and being restarted. Part of every
 * application's own state, so it needs no capability. The value is kept per
 * application name, so it follows the application across a restart.
 *
 * @param value Value to retain.
 */
__syscall void svc_checkpoint_set(uint32_t value);

/**
 * @brief Read back the calling application's checkpoint value.
 *
 * Own state, so it is deliberately ungated.
 *
 * @return The value last stored by @ref svc_checkpoint_set, or zero when the
 *         application has stored none.
 */
__syscall uint32_t svc_checkpoint_get(void);

/**
 * @brief Emit a burst of pulses on the counter stimulus. Needs @ref APP_CAP_COUNTER.
 *
 * @param n Number of pulses to emit.
 * @retval >=0 the pulses emitted, -EPERM without the capability, -ENODEV if absent.
 */
__syscall int svc_pulse_emit(uint32_t n);

/**
 * @brief Read the hardware pulse counter. Needs @ref APP_CAP_COUNTER.
 *
 * @param count Destination for the current count.
 * @retval 0 on success, -EPERM without the capability, -ENODEV if absent.
 */
__syscall int svc_pulse_count(int32_t *count);

/**
 * @brief Drive a motor-control PWM channel. Needs @ref APP_CAP_MOTOR.
 *
 * A channel the application last set to a non-zero duty is set to zero duty
 * when the application is reclaimed.
 *
 * @param channel MCPWM output channel.
 * @param duty_pct Duty cycle, 0..100 percent.
 * @retval 0 on success, -EPERM without the capability, -EBUSY if the service
 *         stayed locked, -EINVAL/-ENODEV otherwise.
 */
__syscall int svc_motor_set(uint32_t channel, uint32_t duty_pct);

/**
 * @brief Read from an I2C device. Needs @ref APP_CAP_I2C and a per-address grant
 *        (@ref app_platform_grant_i2c).
 *
 * @param addr 7-bit device address.
 * @param buf  Receives the bytes.
 * @param len  Number of bytes (bounded by the platform).
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_I2C.
 * @retval -EACCES with the capability but without a grant for this address.
 */
__syscall int svc_i2c_read(uint16_t addr, uint8_t *buf, size_t len);

/**
 * @brief Write to an I2C device. Needs @ref APP_CAP_I2C and a per-address
 *        grant (@ref app_platform_grant_i2c).
 *
 * @param addr 7-bit device address.
 * @param buf  Bytes to send.
 * @param len  Number of bytes (bounded by the platform).
 *
 * @retval 0 Success.
 * @retval -EPERM The application does not hold @ref APP_CAP_I2C.
 * @retval -EACCES The capability is held but this address was not granted.
 * @retval -EINVAL @p len is over the platform's transfer bound.
 * @retval -ENODEV The board exposes no application I2C bus.
 * @retval -EIO The device did not acknowledge.
 */
__syscall int svc_i2c_write(uint16_t addr, const uint8_t *buf, size_t len);

/**
 * @brief Write then read back in one transaction, without releasing the bus.
 *
 * The usual way to address a register: send the register number, then read its
 * value with a repeated start so another application cannot interleave.
 *
 * @param addr   7-bit device address.
 * @param tx     Bytes to send first.
 * @param tx_len Number of bytes to send.
 * @param rx     Receives the reply.
 * @param rx_len Number of bytes to read.
 *
 * @retval 0 Success.
 * @retval -EPERM The application does not hold @ref APP_CAP_I2C.
 * @retval -EACCES The capability is held but this address was not granted.
 * @retval -EINVAL Either length is over the platform's transfer bound.
 * @retval -ENODEV The board exposes no application I2C bus.
 * @retval -EIO The device did not acknowledge.
 */
__syscall int svc_i2c_write_read(uint16_t addr, const uint8_t *tx, size_t tx_len,
				 uint8_t *rx, size_t rx_len);

/** @brief Grant an application access to one I2C address. */
void app_platform_grant_i2c(int id, uint16_t addr);

/**
 * @brief Full-duplex SPI transfer. Needs @ref APP_CAP_SPI and a grant
 *        (@ref app_platform_grant_spi).
 *
 * @param tx  Bytes to send.
 * @param rx  Receives the bytes clocked in.
 * @param len Transfer length (bounded by the platform).
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_SPI.
 * @retval -EACCES with the capability but without a grant for the bus.
 */
__syscall int svc_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

/** @brief Grant or revoke an application's SPI access. */
void app_platform_grant_spi(int id, bool allow);

/**
 * @brief Write to the secondary UART. Needs @ref APP_CAP_UART.
 *
 * @param buf Bytes to write.
 * @param len Length (bounded by the platform).
 * @retval >=0 bytes written, -EPERM without the capability.
 */
__syscall int svc_uart_write(const uint8_t *buf, size_t len);

/**
 * @brief Read available bytes from the secondary UART. Needs @ref APP_CAP_UART.
 *
 * Polled and non-blocking: returns however many bytes are ready, up to @p len.
 *
 * @param buf Receives the bytes.
 * @param len Buffer size (bounded by the platform).
 * @retval >=0 bytes read (may be 0), -EPERM without the capability.
 */
__syscall int svc_uart_read(uint8_t *buf, size_t len);

/** @brief Parity for @ref svc_uart_configure. */
enum app_uart_parity {
	APP_UART_PARITY_NONE = 0,
	APP_UART_PARITY_ODD,
	APP_UART_PARITY_EVEN,
};

/**
 * @brief Set the secondary UART's line parameters. Needs @ref APP_CAP_UART.
 *
 * The platform owns the port and keeps it open; this only changes how it is
 * framed, so an application can talk to a device that does not run at the
 * configured default without reaching the driver itself. The settings stay
 * until another call or @ref svc_uart_close, and are restored when the
 * application is reclaimed, so one application cannot leave the port
 * misconfigured for the next.
 *
 * @param baud     Line rate in bits per second.
 * @param parity   One of @ref app_uart_parity.
 * @param stop_bits 1 or 2.
 * @param data_bits 5 to 9.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_UART.
 * @retval -EINVAL on an unsupported combination.
 * @retval -ENODEV when no secondary UART is chosen.
 * @retval -ENOTSUP when the driver cannot be reconfigured at runtime.
 */
__syscall int svc_uart_configure(uint32_t baud, uint8_t parity,
				 uint8_t stop_bits, uint8_t data_bits);

/**
 * @brief Restore the secondary UART's boot configuration. Needs
 *        @ref APP_CAP_UART.
 *
 * The port itself stays open for the platform; this drops whatever
 * @ref svc_uart_configure changed, so a later reader sees the devicetree
 * settings again.
 *
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_UART.
 * @retval -ENODEV when no secondary UART is chosen.
 */
__syscall int svc_uart_close(void);

/**
 * @brief Header at the front of a ring bound with @ref svc_ring_bind.
 *
 * The application writes at @c head and the platform consumes from @c tail
 * when sending; the platform writes at @c head and the application consumes
 * from @c tail when receiving. Both live in the application's own memory, so
 * the platform re-reads and range-checks them on every use and reports
 * @c -EBADMSG rather than trusting a value that cannot be right.
 */
struct app_ring_hdr {
	volatile uint32_t head;  /**< index one past the newest byte */
	volatile uint32_t tail;  /**< index of the oldest unread byte */
	uint32_t reserved[2];    /**< pad the data to an aligned offset */
};

/**
 * @brief Bind a bulk-transfer ring into the application's memory. Needs
 *        @ref APP_CAP_UART.
 *
 * The platform allocates the buffer and maps it into the calling
 * application's domain, so the application fills or drains it directly and
 * crosses the syscall boundary once per batch rather than once per transfer.
 * The returned address points at a @ref app_ring_hdr followed by @p out_cap
 * data bytes.
 *
 * The mapping becomes a memory partition, so the platform rounds the request
 * up to a power of two and costs the application one partition slot until it
 * unbinds. One ring per application.
 *
 * @param bytes         Data capacity wanted, before rounding.
 * @param[out] out_addr Receives the ring address in the application's space.
 * @param[out] out_cap  Receives the usable data capacity.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_UART.
 * @retval -EINVAL on a zero or oversized request.
 * @retval -EEXIST when the application already bound a ring.
 * @retval -ENOMEM when the buffer could not be allocated.
 */
__syscall int svc_ring_bind(size_t bytes, uintptr_t *out_addr, size_t *out_cap);

/**
 * @brief Release the application's ring. Needs @ref APP_CAP_UART.
 *
 * The mapping is removed and the buffer freed, so the address returned by
 * @ref svc_ring_bind must not be touched afterwards. A reclaimed application
 * releases its ring automatically.
 *
 * @retval 0 on success, -EPERM without the capability, -EINVAL with no ring.
 */
__syscall int svc_ring_unbind(void);

/**
 * @brief Send everything queued in the ring. Needs @ref APP_CAP_UART.
 *
 * One call moves the whole batch the application wrote since the last flush,
 * so a payload of any size costs a single crossing of the boundary.
 *
 * @retval >=0 bytes sent.
 * @retval -EPERM without @ref APP_CAP_UART.
 * @retval -EINVAL with no ring bound.
 * @retval -EBADMSG when the ring's indices are out of range.
 * @retval -ENODEV when no secondary UART is chosen.
 */
__syscall int svc_ring_flush(void);

/**
 * @brief Take everything waiting on the port into the ring. Needs
 *        @ref APP_CAP_UART.
 *
 * Fills up to one byte short of the application's read position, so a full
 * ring stays distinguishable from an empty one.
 *
 * @retval >=0 bytes received.
 * @retval -EPERM without @ref APP_CAP_UART.
 * @retval -EINVAL with no ring bound.
 * @retval -EBADMSG when the ring's indices are out of range.
 * @retval -ENODEV when no secondary UART is chosen.
 */
__syscall int svc_ring_fill(void);

/**
 * @brief Scan for a BLE peer advertising a given name. Needs @ref APP_CAP_BLE.
 *
 * Lock-serialized: the platform owns the controller, so concurrent scans from
 * several applications are serialized. The application gets only a yes or no and
 * never touches the BLE stack.
 *
 * @param name       Advertised complete or shortened name to match.
 * @param timeout_ms How long to scan, in milliseconds.
 * @retval 1 if the peer was seen, 0 if not, -EPERM without the capability,
 *         negative errno on error.
 */
__syscall int svc_ble_scan(const char *name, uint32_t timeout_ms);

/**
 * @brief Connect to a heart-rate peer and read one measurement.
 * Needs @ref APP_CAP_BLE.
 *
 * The platform scans for a peer advertising the Heart Rate Service, connects,
 * discovers the measurement characteristic, subscribes and returns the first
 * value. The whole GATT client lives in the platform; the application gets back
 * a single reading and links none of the BLE stack.
 *
 * @param timeout_ms How long to wait for a reading, in milliseconds.
 * @param out_bpm    Receives the heart-rate value in beats per minute.
 * @retval 0 on success, -EPERM without the capability, -ETIMEDOUT if no reading.
 */
__syscall int svc_ble_hr_read(uint32_t timeout_ms, uint8_t *out_bpm);

/**
 * @brief Fetch a signed application from a BLE peer. Needs @ref APP_CAP_BLE.
 *
 * Connects to the peer advertising as @p name, pulls its manifest, signature
 * and module over GATT and leaves all three in the registry's staged files
 * for @p name, so the installed image is never overwritten. Nothing is
 * verified or launched here: the transfer is untrusted, and the caller checks
 * the staged signature and module hash, then installs them with
 * app_registry_commit().
 *
 * @param name       Peer's advertised name, which is also the application name
 *                   the manifest must claim.
 * @param timeout_ms Per-exchange timeout.
 * @param[out] out_len Receives the number of module bytes stored. May be NULL.
 *
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_BLE, or if the manifest names a
 *         different application than @p name.
 * @retval -EBADMSG if the manifest is malformed or the peer sent fewer module
 *         bytes than it declared.
 * @retval -EFBIG if the module is larger than the loader accepts.
 * @retval -EBUSY if another BLE request is in flight.
 * @retval -ETIMEDOUT if the peer did not answer in time.
 */
__syscall int svc_ble_ota_fetch(const char *name, uint32_t timeout_ms,
				uint32_t *out_len);

/**
 * @brief Maximum payload one generic GATT read or write may carry.
 */
#define APP_BLE_GATT_MAX 64

/**
 * @brief Read a characteristic from a BLE peer by UUID. Needs
 *        @ref APP_CAP_BLE and a service grant
 *        (@ref app_platform_grant_ble_gatt).
 *
 * The platform connects to @p name, discovers @p svc_uuid and @p chr_uuid and
 * reads the value. An application names UUIDs and never sees a connection or
 * an attribute handle, so it cannot reach a characteristic outside the service
 * it was granted. UUIDs are given as 16-bit values, or as 128-bit values in
 * little-endian byte order when @p svc_uuid_len is 16.
 *
 * @param name         Peer's advertised name.
 * @param svc_uuid     Service UUID bytes.
 * @param svc_uuid_len 2 or 16.
 * @param chr_uuid     Characteristic UUID bytes.
 * @param chr_uuid_len 2 or 16.
 * @param buf          Receives the value.
 * @param cap          Buffer size, at most @ref APP_BLE_GATT_MAX.
 * @param timeout_ms   How long to wait.
 *
 * @retval >=0 the number of bytes read.
 * @retval -EPERM without @ref APP_CAP_BLE.
 * @retval -EACCES if @p svc_uuid was not granted to this application.
 * @retval -EINVAL for a malformed UUID length or an oversized buffer.
 * @retval -ENOENT if the peer does not expose the service or characteristic.
 * @retval -EBUSY if another BLE request is in flight.
 * @retval -ETIMEDOUT if the peer did not answer in time.
 */
__syscall int svc_ble_gatt_read(const char *name,
				const uint8_t *svc_uuid, size_t svc_uuid_len,
				const uint8_t *chr_uuid, size_t chr_uuid_len,
				uint8_t *buf, size_t cap, uint32_t timeout_ms);

/**
 * @brief Write a characteristic on a BLE peer by UUID. Needs
 *        @ref APP_CAP_BLE and a service grant
 *        (@ref app_platform_grant_ble_gatt).
 *
 * Scoped exactly as @ref svc_ble_gatt_read: the granted service UUID bounds
 * what the application may write.
 *
 * @param name         Peer's advertised name.
 * @param svc_uuid     Service UUID bytes.
 * @param svc_uuid_len 2 or 16.
 * @param chr_uuid     Characteristic UUID bytes.
 * @param chr_uuid_len 2 or 16.
 * @param buf          Value to write.
 * @param len          Value length, at most @ref APP_BLE_GATT_MAX.
 * @param timeout_ms   How long to wait.
 *
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_BLE.
 * @retval -EACCES if @p svc_uuid was not granted to this application.
 * @retval -EINVAL for a malformed UUID length or an oversized value.
 * @retval -ENOENT if the peer does not expose the service or characteristic.
 * @retval -EBUSY if another BLE request is in flight.
 * @retval -ETIMEDOUT if the peer did not answer in time.
 */
__syscall int svc_ble_gatt_write(const char *name,
				 const uint8_t *svc_uuid, size_t svc_uuid_len,
				 const uint8_t *chr_uuid, size_t chr_uuid_len,
				 const uint8_t *buf, size_t len,
				 uint32_t timeout_ms);

/**
 * @brief Allow an application to reach one GATT service UUID.
 *
 * Holding @ref APP_CAP_BLE lets an application scan and use the profiles the
 * platform implements; reaching a service through the generic GATT client
 * additionally requires that service's UUID to be granted here. Call once per
 * service, up to CONFIG_APP_PLATFORM_BLE_GATT_MAX_SERVICES.
 *
 * @param id       Application id from @ref app_platform_launch.
 * @param uuid     Service UUID bytes.
 * @param uuid_len 2 or 16.
 */
void app_platform_grant_ble_gatt(int id, const uint8_t *uuid, size_t uuid_len);

/**
 * @brief Maximum characteristics one application may serve.
 */
#define APP_BLE_SERVE_MAX_CHRC 4

/** @brief A characteristic the application asks the platform to serve. */
struct app_ble_chrc {
	uint8_t uuid[16];   /**< characteristic UUID bytes, little-endian */
	uint8_t uuid_len;   /**< 2 or 16 */
	uint8_t readable;   /**< non-zero to allow reads */
	uint8_t writable;   /**< non-zero to allow writes */
	uint8_t notifiable; /**< non-zero to allow @ref svc_ble_notify */
};

/**
 * @brief Serve a GATT service and start advertising. Needs @ref APP_CAP_BLE
 *        and a grant for @p svc_uuid (@ref app_platform_grant_ble_gatt).
 *
 * The platform registers the service, owns the value buffers and the
 * connection, and advertises under @p name so a phone or another central can
 * connect. The application never sees a connection or an attribute handle: it
 * publishes values with @ref svc_ble_serve_set, and a peer's write arrives as
 * an @ref APP_EVENT_GATT_WRITE event carrying the characteristic index, which
 * the application reads with @ref svc_event_wait like any other input.
 *
 * @param name     Advertised name.
 * @param svc_uuid Service UUID bytes, little-endian.
 * @param uuid_len 2 or 16.
 * @param chrc     Characteristics to serve, in index order.
 * @param n_chrc   How many, at most @ref APP_BLE_SERVE_MAX_CHRC.
 *
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_BLE.
 * @retval -EACCES if @p svc_uuid was not granted to this application.
 * @retval -EINVAL for a malformed UUID length or too many characteristics.
 * @retval -EBUSY if another application is already serving.
 */
__syscall int svc_ble_serve_start(const char *name, const uint8_t *svc_uuid,
				  size_t uuid_len,
				  const struct app_ble_chrc *chrc,
				  size_t n_chrc);

/**
 * @brief Publish a served characteristic's value. Needs @ref APP_CAP_BLE.
 *
 * The platform answers a peer's read from this value, so the application sets
 * it and does not take part in the exchange.
 *
 * @param idx Characteristic index, as passed to @ref svc_ble_serve_start.
 * @param buf Value bytes.
 * @param len Length, at most @ref APP_BLE_GATT_MAX.
 *
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_BLE.
 * @retval -EINVAL for an unknown index or an oversized value.
 * @retval -EACCES if this application is not the one serving.
 */
__syscall int svc_ble_serve_set(int idx, const uint8_t *buf, size_t len);

/**
 * @brief Read back a served characteristic, including what a peer wrote.
 *        Needs @ref APP_CAP_BLE.
 *
 * @param idx Characteristic index.
 * @param buf Receives the value.
 * @param cap Buffer size.
 *
 * @retval >=0 the number of bytes copied.
 * @retval -EPERM without @ref APP_CAP_BLE.
 * @retval -EINVAL for an unknown index.
 * @retval -EACCES if this application is not the one serving.
 */
__syscall int svc_ble_serve_get(int idx, uint8_t *buf, size_t cap);

/**
 * @brief Stop serving and stop advertising. Needs @ref APP_CAP_BLE.
 *
 * @retval 0 on success, -EPERM without the capability.
 */
__syscall int svc_ble_serve_stop(void);

/**
 * @brief Notify a subscribed peer of a characteristic's value. Needs
 *        @ref APP_CAP_BLE.
 *
 * Pushes the value instead of waiting for the peer to read it. The
 * characteristic must have been declared with @c notifiable, and a peer must
 * have subscribed; without a subscriber the call reports -ENOTCONN rather than
 * queueing, so an application does not build a backlog for a peer that is not
 * listening. The value is stored as if by @ref svc_ble_serve_set, so a later
 * read returns the same bytes.
 *
 * @param idx Characteristic index, in the order given to
 *            @ref svc_ble_serve_start.
 * @param buf Bytes to send.
 * @param len Length, at most @ref APP_BLE_GATT_MAX.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_BLE.
 * @retval -EINVAL on a bad index or length.
 * @retval -EACCES if the characteristic is not notifiable.
 * @retval -ENOTCONN with no connected or subscribed peer.
 */
__syscall int svc_ble_notify(int idx, const void *buf, size_t len);

/**
 * @brief Report whether a peer is connected to the served application.
 *        Needs @ref APP_CAP_BLE.
 *
 * @retval 1 when a peer is connected, 0 when not.
 * @retval -EPERM without @ref APP_CAP_BLE.
 */
__syscall int svc_ble_is_connected(void);

/**
 * @brief Start or stop advertising the served application. Needs
 *        @ref APP_CAP_BLE.
 *
 * @ref svc_ble_serve_start already advertises, so this is for an application
 * that wants to go quiet without dropping its service, or to advertise again
 * after stopping. The platform re-advertises by itself when a peer
 * disconnects.
 *
 * @param on          Non-zero to advertise, zero to stop.
 * @param interval_ms Advertising interval in milliseconds, or 0 for the
 *                    platform default. Clamped to what the controller allows.
 * @retval 0 on success.
 * @retval -EPERM without @ref APP_CAP_BLE.
 * @retval -EINVAL when the application is not serving.
 */
__syscall int svc_ble_advertise(int on, uint32_t interval_ms);

/**
 * @brief Send a datagram to the 802.15.4 peer. Needs @ref APP_CAP_RADIO.
 *
 * The platform owns the radio and the socket; the application hands off a copied
 * payload and a supervisor thread transmits it.
 *
 * @param buf Payload bytes.
 * @param len Payload length (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_radio_send(const void *buf, size_t len);

/**
 * @brief Receive a datagram from the 802.15.4 peer. Needs @ref APP_CAP_RADIO.
 *
 * @param buf        Receives the bytes.
 * @param len        Buffer size (bounded by the platform).
 * @param timeout_ms Milliseconds to wait.
 * @retval >=0 the received length, -EPERM without the capability, -EAGAIN on
 *         timeout.
 */
__syscall int svc_radio_recv(void *buf, size_t len, uint32_t timeout_ms);

/**
 * @brief Send a CAN frame. Needs @ref APP_CAP_CAN.
 *
 * The controller runs in loopback, so a sent frame can be received back on the
 * same board. The application never holds the bus.
 *
 * @param id  CAN identifier.
 * @param buf Up to 8 payload bytes.
 * @param len Payload length.
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_can_send(uint32_t id, const uint8_t *buf, size_t len);

/**
 * @brief Receive a CAN frame. Needs @ref APP_CAP_CAN.
 *
 * @param id_out     Receives the frame identifier.
 * @param buf        Receives the payload.
 * @param cap        Buffer size.
 * @param timeout_ms Milliseconds to wait.
 * @retval >=0 the payload length, -EPERM without the capability, -EAGAIN on
 *         timeout.
 */
__syscall int svc_can_recv(uint32_t *id_out, uint8_t *buf, size_t cap,
			   uint32_t timeout_ms);

/**
 * @brief Copy a buffer through the DMA engine. Needs @ref APP_CAP_DMA.
 *
 * The platform owns the channel and DMA-capable buffers and handles cache
 * coherency; the application hands in bytes and gets the copy back.
 * Lock-serialized.
 *
 * @param src Source bytes.
 * @param dst Receives the copied bytes.
 * @param len Length (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability, negative errno otherwise.
 */
__syscall int svc_dma_copy(const uint8_t *src, uint8_t *dst, size_t len);

/**
 * @brief Map a region of external RAM into the calling app. Needs @ref APP_CAP_PSRAM.
 *
 * The platform allocates a region of external RAM (PSRAM) and adds it as a
 * partition to the calling application's memory domain, so the application
 * accesses it directly at full speed with no per-access syscall. The size is
 * rounded up to a power of two for the memory-protection hardware. The region is
 * zeroed before it is mapped. It is released automatically when the application is reclaimed, or by
 * @ref svc_psram_unmap.
 *
 * @param size     Requested size in bytes.
 * @param out_addr Receives the base address of the mapped region.
 * @param out_size Receives the actual (rounded-up) size.
 * @retval 0 on success, -EPERM without the capability, -ENOMEM if external RAM
 *         is exhausted, -ENOTSUP if no external RAM is present.
 */
__syscall int svc_psram_map(size_t size, uintptr_t *out_addr, size_t *out_size);

/**
 * @brief Release the calling application's mapped external RAM region.
 * Needs @ref APP_CAP_PSRAM.
 *
 * @retval 0 on success, -EPERM without the capability, -EINVAL if nothing mapped.
 */
__syscall int svc_psram_unmap(void);

#include <zephyr/syscalls/app_services.h>

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_SERVICES_H_ */
