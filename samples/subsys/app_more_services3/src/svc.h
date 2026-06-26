/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES3_SVC_H_
#define APP_MORE_SERVICES3_SVC_H_

#include <zephyr/kernel.h>

/*
 * A third batch of services touching the analog and PWM subsystems, now
 * multi-channel with per-channel grant enforcement:
 *
 *   svc_adc_read - read a raw value from ADC channel index @ch (open).
 *   svc_pwm_set  - set duty 0..100 percent on PWM channel index @ch. The app may
 *                  only drive channels in its granted set; an ungranted channel
 *                  is rejected at the syscall boundary.
 *
 * The platform owns every channel and grants a subset to the app. An app reads
 * analog values and dims outputs, reaching the ADC and PWM subsystems only
 * through service syscalls and only on the channels it was granted.
 */

/* Open: read raw ADC value from channel index @ch. Returns value or negative. */
__syscall int svc_adc_read(int ch);

/* Scoped: set PWM duty 0..100 percent on granted channel index @ch. */
__syscall int svc_pwm_set(int ch, uint8_t percent);

/* Supervisor-only: grant the app a set of PWM channels (bit N = channel N). */
void svc_pwm_grant(uint32_t channel_mask);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES3_SVC_H_ */
