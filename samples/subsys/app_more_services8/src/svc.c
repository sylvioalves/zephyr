/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

#define PULSE_MAX  1000
#define STIM_HZ    1000U

/*
 * The platform owns the pulse counter and a PWM stimulus output. The PWM signal
 * is routed on the same pin as the counter's signal input, so a fixed-frequency
 * burst is self-counted. Emitting N pulses = running the PWM for N periods.
 */
static const struct device *const pcnt_dev = DEVICE_DT_GET(DT_NODELABEL(pcnt));
static const struct pwm_dt_spec stim = PWM_DT_SPEC_GET(DT_ALIAS(pcnt_stim));

void svc_pulse_install(void)
{
	/* Idle: PWM output held low until a burst is requested. */
	if (device_is_ready(stim.dev)) {
		(void)pwm_set_dt(&stim, PWM_HZ(STIM_HZ), 0);
	}
}

int z_impl_svc_pulse_emit(uint32_t n)
{
	if (!device_is_ready(stim.dev) || n > PULSE_MAX) {
		return -EINVAL;
	}

	/* Run a 50% square wave for exactly @n periods, then stop. */
	uint32_t period = PWM_HZ(STIM_HZ);

	(void)pwm_set_dt(&stim, period, period / 2U);
	k_msleep((int32_t)((n * 1000U) / STIM_HZ));
	(void)pwm_set_dt(&stim, period, 0);

	return (int)n;
}

int z_impl_svc_pulse_count(int32_t *count)
{
	if (!device_is_ready(pcnt_dev)) {
		return -ENODEV;
	}

	int rc = sensor_sample_fetch(pcnt_dev);

	if (rc != 0) {
		return rc;
	}

	struct sensor_value v;

	rc = sensor_channel_get(pcnt_dev, SENSOR_CHAN_ROTATION, &v);
	if (rc != 0) {
		return rc;
	}

	*count = v.val1;
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_pulse_emit(uint32_t n)
{
	if (n > PULSE_MAX) {
		return -EINVAL;
	}
	return z_impl_svc_pulse_emit(n);
}
#include <zephyr/syscalls/svc_pulse_emit_mrsh.c>

static inline int z_vrfy_svc_pulse_count(int32_t *count)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(count, sizeof(*count)));
	return z_impl_svc_pulse_count(count);
}
#include <zephyr/syscalls/svc_pulse_count_mrsh.c>
#endif
