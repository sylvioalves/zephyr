/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app driving the motor-control PWM (MCPWM) through services from user mode.
 * The platform owns the MCPWM controller; the app ramps the duty cycle on two
 * output channels the way a motor driver would, never touching the controller
 * directly.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	static const uint32_t duties[] = { 0, 25, 50, 75, 100, 50, 0 };

	for (int i = 0; i < (int)ARRAY_SIZE(duties); i++) {
		int rc0 = svc_motor_set(0, duties[i]);
		int rc1 = svc_motor_set(1, 100 - duties[i]);

		printk("  [app] ch0=%u%% (rc=%d), ch1=%u%% (rc=%d)\n",
		       duties[i], rc0, 100 - duties[i], rc1);

		k_msleep(300);
	}

	svc_motor_set(0, 0);
	svc_motor_set(1, 0);
	printk("  [app] done - ramped MCPWM on two channels\n");
}

int main(void)
{
	printk("\n*** more services 9: motor pwm / MCPWM (C6) ***\n");

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
