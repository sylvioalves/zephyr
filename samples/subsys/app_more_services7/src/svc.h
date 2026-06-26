/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES7_SVC_H_
#define APP_MORE_SERVICES7_SVC_H_

#include <zephyr/kernel.h>

/*
 * A seventh batch: a physical input-interrupt event seam. The platform owns the
 * user button, configures its edge interrupt, and pushes an event into a
 * per-app queue on every press. The app blocks waiting for press events without
 * ever touching the GPIO controller - the same async push shape used for BLE
 * notifications and timer expiry, here driven by a real pin.
 *
 *   svc_button_wait  - block until the next button press; returns the running
 *                      press count, or -EAGAIN on timeout.
 *   svc_led_set      - turn the platform-owned status LED on or off (scoped
 *                      output, paired with the input above).
 */

/* Brokered event seam: wait up to @timeout_ms for a press; returns press count. */
__syscall int svc_button_wait(int32_t timeout_ms);

/* Scoped output: drive the status LED (@on != 0 lights it). */
__syscall int svc_led_set(int on);

/* Supervisor-only: bind the button irq and the LED before launching the app. */
void svc_input_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES7_SVC_H_ */
