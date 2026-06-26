/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES8_SVC_H_
#define APP_MORE_SERVICES8_SVC_H_

#include <zephyr/kernel.h>

/*
 * An eighth batch: a hardware pulse counter. The platform owns the PCNT unit and
 * a stimulus output pin muxed to the same line as the counter's signal input, so
 * pulses are generated internally with no wiring. The app asks the platform to
 * emit N pulses, then reads the accumulated edge count - all from user mode.
 *
 *   svc_pulse_emit  - emit @n pulses on the platform-owned stimulus pin.
 *   svc_pulse_count - read the pulse counter's accumulated edge count.
 */

/* Scoped: generate @n pulses into the counter's signal line. */
__syscall int svc_pulse_emit(uint32_t n);

/* Open: latch and return the counter's accumulated edge count. */
__syscall int svc_pulse_count(int32_t *count);

/* Supervisor-only: configure the stimulus pin before launching the app. */
void svc_pulse_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES8_SVC_H_ */
