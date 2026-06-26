/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/internal/syscall_handler.h>

/* ADC channels declared under /zephyr,user. */
#define ADC_NODE DT_PATH(zephyr_user)
static const struct adc_dt_spec adc_ch[] = {
	ADC_DT_SPEC_GET_BY_IDX(ADC_NODE, 0),
	ADC_DT_SPEC_GET_BY_IDX(ADC_NODE, 1),
};
#define ADC_COUNT ARRAY_SIZE(adc_ch)

/* PWM outputs: two channels the app can drive, if granted. */
static const struct pwm_dt_spec pwm_ch[] = {
	PWM_DT_SPEC_GET(DT_ALIAS(app_pwm0)),
	PWM_DT_SPEC_GET(DT_ALIAS(app_pwm1)),
};
#define PWM_COUNT ARRAY_SIZE(pwm_ch)

/*
 * Per-channel grant: bit N set means the app may drive PWM channel N. The
 * platform sets this before launching the app; the syscall boundary rejects any
 * channel whose bit is clear, the same way a kobject permission would.
 */
static uint32_t pwm_grant;

void svc_pwm_grant(uint32_t channel_mask)
{
	pwm_grant = channel_mask;
}

void svc_init(void)
{
	for (int i = 0; i < ADC_COUNT; i++) {
		if (adc_is_ready_dt(&adc_ch[i])) {
			adc_channel_setup_dt(&adc_ch[i]);
		}
	}
}

int z_impl_svc_adc_read(int ch)
{
	if (ch < 0 || ch >= (int)ADC_COUNT || !adc_is_ready_dt(&adc_ch[ch])) {
		return -ENODEV;
	}
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	(void)adc_sequence_init_dt(&adc_ch[ch], &seq);
	int rc = adc_read_dt(&adc_ch[ch], &seq);

	return (rc == 0) ? (int)raw : rc;
}

int z_impl_svc_pwm_set(int ch, uint8_t percent)
{
	if (ch < 0 || ch >= (int)PWM_COUNT || !pwm_is_ready_dt(&pwm_ch[ch])) {
		return -ENODEV;
	}
	if (percent > 100) {
		percent = 100;
	}
	uint32_t pulse = (uint32_t)((uint64_t)pwm_ch[ch].period * percent / 100);

	return pwm_set_pulse_dt(&pwm_ch[ch], pulse);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_adc_read(int ch)
{
	return z_impl_svc_adc_read(ch);
}
#include <zephyr/syscalls/svc_adc_read_mrsh.c>

static inline int z_vrfy_svc_pwm_set(int ch, uint8_t percent)
{
	/* Reject any channel the app was not granted, at the boundary. */
	if (ch < 0 || ch >= (int)PWM_COUNT || (pwm_grant & BIT(ch)) == 0U) {
		return -EPERM;
	}
	return z_impl_svc_pwm_set(ch, percent);
}
#include <zephyr/syscalls/svc_pwm_set_mrsh.c>
#endif
