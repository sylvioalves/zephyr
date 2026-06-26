/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using the hardware pulse counter through services from user mode. The
 * platform owns the PCNT unit and a stimulus pin wired to the counter's signal
 * input. The app asks for bursts of pulses and reads back the accumulated edge
 * count, never touching the counter or the gpio directly.
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

	for (int round = 0; round < 4; round++) {
		uint32_t burst = 10 * (round + 1);
		int emitted = svc_pulse_emit(burst);

		int32_t count = 0;
		int rc = svc_pulse_count(&count);

		if (rc == 0) {
			printk("  [app] round %d: emitted %d pulses, counter = %d\n",
			       round, emitted, count);
		} else {
			printk("  [app] round %d: count rc=%d\n", round, rc);
		}

		k_msleep(200);
	}

	printk("  [app] done - used pulse emit + counter read\n");
}

int main(void)
{
	printk("\n*** more services 8: pulse counter (C6) ***\n");

	svc_pulse_install();

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
