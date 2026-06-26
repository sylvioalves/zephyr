/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app that uses a broad set of platform services, each a different Zephyr
 * subsystem: the hardware RNG, GPIO input (button) and output (LED), a hardware
 * counter used as a one-shot timer with its expiry delivered to the app, and
 * the system time. The app runs in user mode and reaches all of it only through
 * service syscalls.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

extern void svc_init(void);

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* RNG: hardware entropy. */
	uint8_t r[4];

	svc_random(r, sizeof(r));
	printk("  [app] random bytes: %02x %02x %02x %02x\n",
	       r[0], r[1], r[2], r[3]);

	/* Time. */
	printk("  [app] uptime=%u ms, cycles=%u\n", svc_uptime(), svc_cycles());

	/* GPIO out: blink the LED a few times. */
	printk("  [app] blinking LED via GPIO service\n");
	for (int i = 0; i < 3; i++) {
		svc_led(1); k_msleep(150);
		svc_led(0); k_msleep(150);
	}

	/* GPIO in: read the button. */
	int b = svc_button();

	printk("  [app] button is %s\n",
	       b == 1 ? "PRESSED" : b == 0 ? "released" : "n/a");

	/* Hardware counter as a one-shot timer; wait for the expiry event. */
	printk("  [app] arming a 500 ms hardware timer...\n");
	if (svc_timer_start(500) == 0) {
		int rc = svc_timer_wait(2000);

		printk("  [app] timer %s (uptime now %u ms)\n",
		       rc == 0 ? "FIRED" : "did not fire", svc_uptime());
	} else {
		printk("  [app] timer start failed\n");
	}

	printk("  [app] done - touched RNG, GPIO in/out, counter, time\n");
}

int main(void)
{
	printk("\n*** more services: RNG, GPIO, hardware timer, time (C6) ***\n");
	svc_init();

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
