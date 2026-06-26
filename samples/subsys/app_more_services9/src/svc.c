/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

#define MOTOR_CHANNELS 2U
#define PERIOD_NS      PWM_HZ(1000)

/* The platform owns the MCPWM controller; the app drives granted channels. */
static const struct device *const mcpwm = DEVICE_DT_GET(DT_NODELABEL(mcpwm0));

/* The two output channels exposed to the app (operator 0: PWM0A and PWM0B). */
static const uint32_t channel_map[MOTOR_CHANNELS] = { 0U, 1U };

int z_impl_svc_motor_set(uint32_t channel, uint32_t duty_pct)
{
	if (!device_is_ready(mcpwm) || channel >= MOTOR_CHANNELS || duty_pct > 100U) {
		return -EINVAL;
	}

	uint32_t pulse = (uint32_t)(((uint64_t)PERIOD_NS * duty_pct) / 100U);

	return pwm_set(mcpwm, channel_map[channel], PERIOD_NS, pulse, 0);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_motor_set(uint32_t channel, uint32_t duty_pct)
{
	if (channel >= MOTOR_CHANNELS || duty_pct > 100U) {
		return -EINVAL;
	}
	return z_impl_svc_motor_set(channel, duty_pct);
}
#include <zephyr/syscalls/svc_motor_set_mrsh.c>
#endif
