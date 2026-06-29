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
 * @brief Write to the raw storage partition. Needs @ref APP_CAP_FLASH.
 *
 * Lock-serialized and bounds-checked against the partition.
 *
 * @param off Offset within the storage partition.
 * @param buf Bytes to write.
 * @param len Number of bytes (bounded by the platform).
 * @retval >=0 bytes written, -EPERM without the capability, negative errno.
 */
__syscall int svc_flash_write(uint32_t off, const uint8_t *buf, size_t len);

/**
 * @brief Erase a span of the raw storage partition. Needs @ref APP_CAP_FLASH.
 *
 * Lock-serialized and bounds-checked against the partition.
 *
 * @param off Offset within the storage partition.
 * @param len Length to erase.
 * @retval 0 on success, -EPERM without the capability, negative errno.
 */
__syscall int svc_flash_erase(uint32_t off, size_t len);

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
 * @brief Set a PWM channel duty cycle in per-mille. Needs @ref APP_CAP_PWM and a
 *        per-channel grant (@ref app_platform_grant_pwm).
 *
 * @param ch       Channel index.
 * @param permille Duty cycle in tenths of a percent (0..1000).
 * @retval 0 on success, -EPERM without the capability or channel grant.
 */
__syscall int svc_pwm_set(int ch, uint32_t permille);

/** @brief Grant an application a set of PWM channels. */
void app_platform_grant_pwm(int id, uint32_t ch_mask);

/**
 * @brief Read an ADC channel. Needs @ref APP_CAP_ADC and a per-channel grant
 *        (@ref app_platform_grant_adc).
 *
 * @param ch Channel index.
 * @retval >=0 the raw sample, -EPERM without the capability or channel grant.
 */
__syscall int svc_adc_read(int ch);

/** @brief Grant an application a set of ADC channels. */
void app_platform_grant_adc(int id, uint32_t ch_mask);

/**
 * @brief Read from an I2C device. Needs @ref APP_CAP_I2C and a per-address grant
 *        (@ref app_platform_grant_i2c).
 *
 * @param addr 7-bit device address.
 * @param buf  Receives the bytes.
 * @param len  Number of bytes (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability or address grant.
 */
__syscall int svc_i2c_read(uint16_t addr, uint8_t *buf, size_t len);

/** @brief Grant an application access to one I2C address. */
void app_platform_grant_i2c(int id, uint16_t addr);

/**
 * @brief Full-duplex SPI transfer. Needs @ref APP_CAP_SPI and a grant
 *        (@ref app_platform_grant_spi).
 *
 * @param tx  Bytes to send.
 * @param rx  Receives the bytes clocked in.
 * @param len Transfer length (bounded by the platform).
 * @retval 0 on success, -EPERM without the capability or grant.
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
 * released automatically when the application is reclaimed, or by
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
