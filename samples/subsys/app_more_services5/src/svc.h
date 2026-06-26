/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES5_SVC_H_
#define APP_MORE_SERVICES5_SVC_H_

#include <zephyr/kernel.h>

/*
 * A fifth batch: a scoped second serial port, the chip die-temperature sensor,
 * and a brokered watchdog feed.
 *
 *   svc_uart_write - send bytes on the platform-owned second UART (scoped, like
 *                    SPI/I2C: the platform owns the controller, the app sends
 *                    bytes).
 *   svc_uart_read  - poll a byte from the second UART; -EAGAIN if none waiting.
 *   svc_die_temp   - the chip core temperature in milli-degrees Celsius (open,
 *                    read-only).
 *   svc_wdt_feed   - feed the app's watchdog channel; the platform installs and
 *                    owns the watchdog, the app proves liveness through this.
 */

/* Scoped: write @len bytes out the second UART. Returns count sent. */
__syscall int svc_uart_write(const uint8_t *buf, size_t len);

/* Scoped: poll one byte in; returns 1 on a byte, -EAGAIN if none. */
__syscall int svc_uart_read(uint8_t *out);

/* Open: core die temperature in milli-degrees C. */
__syscall int svc_die_temp(int32_t *milli_c);

/* Brokered: prove liveness on the app's watchdog channel. */
__syscall int svc_wdt_feed(void);

/* Supervisor-only: install and start the watchdog before launching the app. */
void svc_wdt_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES5_SVC_H_ */
