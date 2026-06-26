/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using a scoped second UART, the die-temperature sensor, and a brokered
 * watchdog, all through service syscalls from user mode. The platform owns the
 * UART controller, the sensor, and the watchdog; the app only sends/receives
 * bytes, reads the temperature, and proves liveness by feeding its channel.
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

	for (int round = 0; round < 5; round++) {
		/* Watchdog: prove liveness this round. */
		int wd = svc_wdt_feed();

		/* Die temperature. */
		int32_t milli_c = 0;
		int rc = svc_die_temp(&milli_c);

		if (rc == 0) {
			printk("  [app] round %d: die temp = %d.%03d C, wdt=%d\n",
			       round, milli_c / 1000, milli_c % 1000, wd);
		} else {
			printk("  [app] round %d: die temp rc=%d, wdt=%d\n",
			       round, rc, wd);
		}

		/* Second UART: emit a line out the scoped port. */
		char line[40];
		int n = snprintk(line, sizeof(line), "temp=%d.%03dC\r\n",
				 milli_c / 1000, milli_c % 1000);
		svc_uart_write((const uint8_t *)line, n);

		/* Poll the scoped port for one input byte. */
		uint8_t in;

		if (svc_uart_read(&in) == 1) {
			printk("  [app] uart rx byte: 0x%02x\n", in);
		}

		k_msleep(500);
	}

	printk("  [app] done - used UART + die temp + watchdog\n");
}

int main(void)
{
	printk("\n*** more services 5: UART + die temp + watchdog (C6) ***\n");

	svc_wdt_install();

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
