/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared PWM and ADC services. Both follow the per-channel grant pattern: an
 * application holding the capability may use only the channels it was granted
 * (app_platform_grant_pwm / app_platform_grant_adc), so the capability admits
 * the service and a per-channel mask scopes it. Channels come from devicetree;
 * each service builds to a no-op stub when its devicetree node is absent.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/adc.h>
#include <errno.h>

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* ---- PWM ---- */

#define PWM_NAME(alias_node) \
	DT_PROP_OR(alias_node, label, DT_NODE_FULL_NAME(alias_node))

#if DT_HAS_ALIAS(app_pwm0)
static const struct pwm_dt_spec pwm_ch[] = {
	PWM_DT_SPEC_GET(DT_ALIAS(app_pwm0)),
#if DT_HAS_ALIAS(app_pwm1)
	PWM_DT_SPEC_GET(DT_ALIAS(app_pwm1)),
#endif
};
#define PWM_COUNT ARRAY_SIZE(pwm_ch)

static const char *const pwm_name[] = {
	PWM_NAME(DT_ALIAS(app_pwm0)),
#if DT_HAS_ALIAS(app_pwm1)
	PWM_NAME(DT_ALIAS(app_pwm1)),
#endif
};
#else
#define PWM_COUNT 0
#endif

static uint32_t pwm_grant[MAX_APPS];

void app_platform_grant_pwm(int id, uint32_t ch_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		pwm_grant[id] = ch_mask;
	}
}

int z_impl_svc_pwm_set(int ch, uint32_t permille)
{
	if (!app_has_cap(APP_CAP_PWM)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || ch < 0 || ch >= (int)PWM_COUNT) {
		return -EINVAL;
	}
	if ((pwm_grant[id] & BIT(ch)) == 0U) {
		return -EACCES;
	}
#if DT_HAS_ALIAS(app_pwm0)
	if (!pwm_is_ready_dt(&pwm_ch[ch])) {
		return -ENODEV;
	}
	uint32_t period = pwm_ch[ch].period;

	return pwm_set_dt(&pwm_ch[ch], period, (period * permille) / 1000U);
#else
	ARG_UNUSED(permille);
	return -ENODEV;
#endif
}

int z_impl_svc_pwm_find(const char *name)
{
	if (!app_has_cap(APP_CAP_PWM)) {
		return -EPERM;
	}
#if DT_HAS_ALIAS(app_pwm0)
	for (int i = 0; i < (int)PWM_COUNT; i++) {
		if (strcmp(pwm_name[i], name) == 0) {
			return i;
		}
	}
#else
	ARG_UNUSED(name);
#endif
	return -ENOENT;
}

/* ---- ADC ---- */

#define ADC_HAS_CH \
	(DT_NODE_EXISTS(DT_PATH(zephyr_user)) && \
	 DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels))

#if ADC_HAS_CH
#define ADC_SPEC(node_id, prop, idx) \
	ADC_DT_SPEC_GET_BY_IDX(node_id, idx),
static const struct adc_dt_spec adc_ch[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, ADC_SPEC)
};
#define ADC_COUNT ARRAY_SIZE(adc_ch)
#else
#define ADC_COUNT 0
#endif

static uint32_t adc_grant[MAX_APPS];

/* Drop the channel grants of a slot being handed to a new application. */
void app_platform_clear_periph_grants(int id)
{
	if (id >= 0 && id < MAX_APPS) {
		pwm_grant[id] = 0U;
		adc_grant[id] = 0U;
	}
}
static bool adc_ready;

void app_platform_grant_adc(int id, uint32_t ch_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		adc_grant[id] = ch_mask;
	}
}

int z_impl_svc_adc_read(int ch)
{
	if (!app_has_cap(APP_CAP_ADC)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || ch < 0 || ch >= (int)ADC_COUNT) {
		return -EINVAL;
	}
	if ((adc_grant[id] & BIT(ch)) == 0U) {
		return -EACCES;
	}
#if ADC_HAS_CH
	if (!adc_ready) {
		for (int i = 0; i < (int)ADC_COUNT; i++) {
			if (adc_is_ready_dt(&adc_ch[i])) {
				adc_channel_setup_dt(&adc_ch[i]);
			}
		}
		adc_ready = true;
	}

	int16_t sample = 0;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	if (adc_sequence_init_dt(&adc_ch[ch], &seq) != 0 ||
	    adc_read_dt(&adc_ch[ch], &seq) != 0) {
		return -EIO;
	}
	return (int)sample;
#else
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_pwm_set(int ch, uint32_t permille)
{
	return z_impl_svc_pwm_set(ch, permille);
}
#include <zephyr/syscalls/svc_pwm_set_mrsh.c>

static inline int z_vrfy_svc_pwm_find(const char *name)
{
	char buf[24];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_pwm_find(buf);
}
#include <zephyr/syscalls/svc_pwm_find_mrsh.c>

static inline int z_vrfy_svc_adc_read(int ch)
{
	return z_impl_svc_adc_read(ch);
}
#include <zephyr/syscalls/svc_adc_read_mrsh.c>
#endif
