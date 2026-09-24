/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared motion-control services: a hardware pulse counter and a motor-control
 * PWM (MCPWM) output. The platform owns both peripherals; an application reads
 * the pulse count, emits a fixed-frequency burst, and drives granted MCPWM
 * channels with a duty cycle. Each service builds to a safe stub when its
 * devicetree node is absent, so a board without the peripheral still builds.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>

#include "app_grants.h"

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* ---- hardware pulse counter ---- */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(pcnt), okay) && DT_HAS_ALIAS(pcnt_stim)
#define PULSE_MAX 1000U
#define STIM_HZ   1000U

static const struct device *const pcnt_dev = DEVICE_DT_GET(DT_NODELABEL(pcnt));
static const struct pwm_dt_spec stim = PWM_DT_SPEC_GET(DT_ALIAS(pcnt_stim));

static int pulse_init(void)
{
	/* Idle: stimulus output held low until a burst is requested. */
	if (device_is_ready(stim.dev)) {
		(void)pwm_set_dt(&stim, PWM_HZ(STIM_HZ), 0);
	}
	return 0;
}
SYS_INIT(pulse_init, APPLICATION, 90);

int z_impl_svc_pulse_emit(uint32_t n)
{
	if (!app_has_cap(APP_CAP_COUNTER)) {
		return -EPERM;
	}
	if (!device_is_ready(stim.dev) || n > PULSE_MAX) {
		return -EINVAL;
	}

	/* Run a 50% square wave for exactly @n periods, then stop. The stimulus
	 * pin is wired to the counter input, so the burst is self-counted.
	 */
	uint32_t period = PWM_HZ(STIM_HZ);

	(void)pwm_set_dt(&stim, period, period / 2U);
	k_msleep((int32_t)((n * 1000U) / STIM_HZ));
	(void)pwm_set_dt(&stim, period, 0);
	return (int)n;
}

int z_impl_svc_pulse_count(int32_t *count)
{
	if (!app_has_cap(APP_CAP_COUNTER)) {
		return -EPERM;
	}
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
#else  /* no pcnt node */
int z_impl_svc_pulse_emit(uint32_t n)
{
	if (!app_has_cap(APP_CAP_COUNTER)) {
		return -EPERM;
	}
	ARG_UNUSED(n);
	return -ENODEV;
}

int z_impl_svc_pulse_count(int32_t *count)
{
	if (!app_has_cap(APP_CAP_COUNTER)) {
		return -EPERM;
	}
	ARG_UNUSED(count);
	return -ENODEV;
}
#endif /* pcnt */

/* ---- motor-control PWM (MCPWM) ---- */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(mcpwm0), okay)
#define MOTOR_CHANNELS 2U
#define MOTOR_PERIOD   PWM_HZ(1000)

static const struct device *const mcpwm = DEVICE_DT_GET(DT_NODELABEL(mcpwm0));
static const uint32_t channel_map[MOTOR_CHANNELS] = { 0U, 1U };
/* Bit N set means this application last drove channel N to a non-zero duty. */
static uint32_t motor_driven[MAX_APPS];
static K_MUTEX_DEFINE(motor_lock);

static void motor_release(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	if (app_svc_lock(&motor_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
	for (uint32_t ch = 0U; ch < MOTOR_CHANNELS; ch++) {
		if ((motor_driven[id] & BIT(ch)) != 0U) {
			(void)pwm_set(mcpwm, channel_map[ch], MOTOR_PERIOD, 0U, 0);
		}
	}
	motor_driven[id] = 0U;
	(void)app_svc_unlock(&motor_lock);
}

static int motor_init(void)
{
	return app_platform_on_reclaim(motor_release);
}
SYS_INIT(motor_init, APPLICATION, 99);

int z_impl_svc_motor_set(uint32_t channel, uint32_t duty_pct)
{
	if (!app_has_cap(APP_CAP_MOTOR)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || !device_is_ready(mcpwm) ||
	    channel >= MOTOR_CHANNELS || duty_pct > 100U) {
		return -EINVAL;
	}

	uint32_t pulse = (uint32_t)(((uint64_t)MOTOR_PERIOD * duty_pct) / 100U);
	int rc;

	if (app_svc_lock(&motor_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	rc = pwm_set(mcpwm, channel_map[channel], MOTOR_PERIOD, pulse, 0);
	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			motor_driven[i] &= ~BIT(channel);
		}
		if (duty_pct != 0U) {
			motor_driven[id] |= BIT(channel);
		}
	}
	(void)app_svc_unlock(&motor_lock);
	return rc;
}
#else  /* no mcpwm0 node */
int z_impl_svc_motor_set(uint32_t channel, uint32_t duty_pct)
{
	if (!app_has_cap(APP_CAP_MOTOR)) {
		return -EPERM;
	}
	ARG_UNUSED(channel); ARG_UNUSED(duty_pct);
	return -ENODEV;
}
#endif /* mcpwm0 */

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_pulse_emit(uint32_t n)
{
	return z_impl_svc_pulse_emit(n);
}
#include <zephyr/syscalls/svc_pulse_emit_mrsh.c>

static inline int z_vrfy_svc_pulse_count(int32_t *count)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(count, sizeof(*count)));
	return z_impl_svc_pulse_count(count);
}
#include <zephyr/syscalls/svc_pulse_count_mrsh.c>

static inline int z_vrfy_svc_motor_set(uint32_t channel, uint32_t duty_pct)
{
	return z_impl_svc_motor_set(channel, duty_pct);
}
#include <zephyr/syscalls/svc_motor_set_mrsh.c>
#endif /* CONFIG_USERSPACE */
