/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app reacting to a physical button through a brokered input-event seam, all
 * from user mode. The platform owns the button irq and the status LED; the app
 * blocks on svc_button_wait() and toggles the LED on each press. With no press
 * within the timeout the round reports idle, so the demo runs unattended.
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

	int led_on = 0;

	printk("  [app] waiting for button presses (sw0)...\n");

	for (int round = 0; round < 5; round++) {
		int rc = svc_button_wait(1500);

		if (rc >= 0) {
			led_on = !led_on;
			svc_led_set(led_on);
			printk("  [app] press #%d -> led %s\n", rc,
			       led_on ? "on" : "off");
		} else {
			printk("  [app] round %d: idle (no press)\n", round);
		}
	}

	svc_led_set(0);
	printk("  [app] done - used button event seam + led\n");
}

int main(void)
{
	printk("\n*** more services 7: button event + led (C6) ***\n");

	svc_input_install();

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
