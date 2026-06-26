/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES3_SVC_H_
#define APP_MORE_SERVICES3_SVC_H_

#include <zephyr/kernel.h>

/*
 * A third batch of services touching the analog and PWM subsystems:
 *
 *   svc_adc_read - read a raw value from an ADC channel (open: read-only).
 *   svc_pwm_set  - set the PWM duty on the controllable output, 0..100 percent
 *                  (scoped in a real system; the platform owns the channel).
 *
 * An app reads analog values and dims an output, reaching the ADC and PWM
 * subsystems only through service syscalls.
 */

/* Open: read raw ADC value from channel index @ch. Returns value or negative. */
__syscall int svc_adc_read(int ch);

/* Set PWM duty 0..100 percent on the controllable channel. */
__syscall int svc_pwm_set(uint8_t percent);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES3_SVC_H_ */
