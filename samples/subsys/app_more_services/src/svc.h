/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES_SVC_H_
#define APP_MORE_SERVICES_SVC_H_

#include <zephyr/kernel.h>

/*
 * A broader set of app-callable services, each touching a different Zephyr
 * subsystem in its proper capability shape:
 *
 *   svc_random   - hardware RNG (open: any app)
 *   svc_button   - GPIO input, the devkit button (open: read-only)
 *   svc_led      - GPIO output (scoped in a real system; open here)
 *   svc_timer_*  - hardware counter as a one-shot timer; the expiry is delivered
 *                  to the app's event queue (arbitrated pool + async-event seam)
 *   svc_uptime   - milliseconds since boot (open)
 *   svc_cycles   - the cycle counter (open)
 */

/* Open: entropy from the hardware RNG. Fills @buf with @len random bytes. */
__syscall int svc_random(uint8_t *buf, size_t len);

/* Open: read the on-board button. Returns 1 pressed, 0 released, <0 error. */
__syscall int svc_button(void);

/* Open/scoped: set the controllable LED pin. */
__syscall int svc_led(int on);

/* Arbitrated: arm a one-shot hardware-counter timer for @ms milliseconds. The
 * expiry is posted to the calling app's event queue (poll with svc_timer_wait).
 */
__syscall int svc_timer_start(uint32_t ms);
/* Wait up to @timeout_ms for the timer to expire. 0 = expired, -EAGAIN = not. */
__syscall int svc_timer_wait(int32_t timeout_ms);

/* Open: time. */
__syscall uint32_t svc_uptime(void);
__syscall uint32_t svc_cycles(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES_SVC_H_ */
