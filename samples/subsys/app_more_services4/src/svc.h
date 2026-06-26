/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES4_SVC_H_
#define APP_MORE_SERVICES4_SVC_H_

#include <zephyr/kernel.h>

/*
 * A fourth batch: the SPI bus (scoped, like I2C) and system hardware info.
 *
 *   svc_spi_xfer  - full-duplex SPI transfer on the platform-owned bus. Scoped
 *                   in a real system to a granted chip-select; the platform owns
 *                   the controller, the app sends/receives bytes.
 *   svc_device_id - the chip's unique hardware id (open, read-only).
 *   svc_reset_cause - why the device last reset (open, read-only system info).
 */

/* Scoped: full-duplex transfer @len bytes (tx -> rx). */
__syscall int svc_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

/* Open: copy up to @len bytes of the unique device id into @buf; returns count. */
__syscall int svc_device_id(uint8_t *buf, size_t len);

/* Open: the reset cause bitmask from the last reset. */
__syscall int svc_reset_cause(uint32_t *cause);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES4_SVC_H_ */
