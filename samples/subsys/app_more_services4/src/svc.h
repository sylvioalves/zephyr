/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES4_SVC_H_
#define APP_MORE_SERVICES4_SVC_H_

#include <zephyr/kernel.h>

/*
 * A fourth batch: the SPI bus (now multi chip-select with per-target grants) and
 * system hardware info.
 *
 *   svc_spi_xfer  - full-duplex transfer on a granted chip-select. The platform
 *                   owns the controller and all chip-selects; the app may only
 *                   talk to the targets it was granted, the rest are rejected at
 *                   the syscall boundary.
 *   svc_device_id - the chip's unique hardware id (open, read-only).
 *   svc_reset_cause - why the device last reset (open, read-only system info).
 */

/* Scoped: full-duplex @len bytes on granted chip-select @cs (tx -> rx). */
__syscall int svc_spi_xfer(int cs, const uint8_t *tx, uint8_t *rx, size_t len);

/* Open: copy up to @len bytes of the unique device id into @buf; returns count. */
__syscall int svc_device_id(uint8_t *buf, size_t len);

/* Open: the reset cause bitmask from the last reset. */
__syscall int svc_reset_cause(uint32_t *cause);

/* Supervisor-only: grant the app a set of chip-selects (bit N = CS index N). */
void svc_spi_grant(uint32_t cs_mask);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES4_SVC_H_ */
