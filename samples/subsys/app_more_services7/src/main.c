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
#include <errno.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	printk("  [app] waiting for button presses (sw0)...\n");

	/* Walk the two granted LED pins; ungranted pin 2 is denied. */
	for (int round = 0; round < 5; round++) {
		int rc = svc_button_wait(1500);
		int pin = round % 2;

		if (rc >= 0) {
			svc_led_set(pin, 1);
			printk("  [app] press #%d -> led pin %d on\n", rc, pin);
		} else {
			svc_led_set(pin, round & 1);
			printk("  [app] round %d: idle, toggled granted pin %d\n",
			       round, pin);
		}
	}

	/* LED pin 2 was NOT granted - the boundary rejects it. */
	int rc2 = svc_led_set(2, 1);

	printk("  [app] led pin 2 -> rc=%d (expected -EPERM=%d, ungranted)\n",
	       rc2, -EPERM);

	svc_led_set(0, 0);
	svc_led_set(1, 0);
	printk("  [app] done - granted LEDs driven, ungranted denied\n");
}

int main(void)
{
	printk("\n*** more services 7: button event + led (C6) ***\n");

	svc_input_install();

	/* Grant LED pins 0 and 1; pin 2 stays denied. */
	svc_led_grant(BIT(0) | BIT(1));

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
