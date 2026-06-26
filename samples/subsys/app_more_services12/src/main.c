/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using a real-time clock through services from user mode. The platform
 * owns the RTC; the app sets the wall-clock and then reads it back a few times
 * to watch it advance, passing only plain scalar fields across the syscall
 * boundary.
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

	/* Set the clock to a known time. */
	int rc = svc_rtc_set(2026, 6, 26, 12, 30, 0);

	printk("  [app] set clock to 2026-06-26 12:30:00 (rc=%d)\n", rc);

	for (int i = 0; i < 4; i++) {
		int y, mo, d, h, mi, s;

		if (svc_rtc_get(&y, &mo, &d, &h, &mi, &s) == 0) {
			printk("  [app] now: %04d-%02d-%02d %02d:%02d:%02d\n",
			       y, mo, d, h, mi, s);
		}
		k_msleep(1100);
	}

	printk("  [app] done - set and read the wall clock\n");
}

int main(void)
{
	printk("\n*** more services 12: real-time clock (C6) ***\n");

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
