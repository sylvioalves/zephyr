/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES9_SVC_H_
#define APP_MORE_SERVICES9_SVC_H_

#include <zephyr/kernel.h>

/*
 * A ninth batch: the motor-control PWM (MCPWM), distinct from the LEDC PWM. The
 * platform owns the MCPWM timer and its output channels; the app sets a duty
 * cycle per channel from user mode, the way a motor driver would ramp an
 * H-bridge. The channel index is validated against the granted set.
 *
 *   svc_motor_set - drive MCPWM @channel at @duty_pct (0..100) percent.
 */

/* Scoped: set output @channel duty to @duty_pct percent (0..100). */
__syscall int svc_motor_set(uint32_t channel, uint32_t duty_pct);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES9_SVC_H_ */
