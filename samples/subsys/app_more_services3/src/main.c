/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using the analog and PWM subsystems through services. It reads two ADC
 * channels and ramps a PWM output from off to full and back, dimming a pin. The
 * app runs in user mode and reaches the ADC and PWM hardware only through
 * service syscalls.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

extern void svc_init(void);

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* ADC: read two analog channels. */
	for (int ch = 0; ch < 2; ch++) {
		int v = svc_adc_read(ch);

		printk("  [app] ADC channel %d = %d\n", ch, v);
	}

	/* PWM: ramp the granted channel (0) up and down. */
	printk("  [app] ramping PWM channel 0 (granted)\n");
	for (int p = 0; p <= 100; p += 25) {
		int rc = svc_pwm_set(0, (uint8_t)p);

		printk("  [app] ch0 duty = %d%% (rc=%d)\n", p, rc);
		k_msleep(150);
	}
	svc_pwm_set(0, 0);

	/* PWM: channel 1 was NOT granted - the boundary rejects it. */
	int rc1 = svc_pwm_set(1, 50);

	printk("  [app] ch1 duty 50%% -> rc=%d (expected -EPERM=%d, ungranted)\n",
	       rc1, -EPERM);

	printk("  [app] done - read ADC, drove granted PWM, denied ungranted\n");
}

int main(void)
{
	printk("\n*** more services 3: ADC + PWM (C6) ***\n");
	svc_init();

	/* Grant the app PWM channel 0 only; channel 1 stays denied. */
	svc_pwm_grant(BIT(0));

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
