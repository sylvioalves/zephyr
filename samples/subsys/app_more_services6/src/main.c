/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using the CAN bus through services from user mode. The platform owns
 * the controller, puts it in loopback, installs a receive filter feeding a
 * per-app queue, and starts it. The app sends frames and polls received ones,
 * passing only a scalar id and a byte payload across the user/kernel boundary.
 * Loopback echoes each sent frame back, so the demo runs with no wiring.
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
		uint8_t tx[4] = { 0x10 + round, 0x20, 0x30, 0x40 };
		uint32_t id = 0x123 + round;

		int sent = svc_can_send(id, tx, sizeof(tx));

		printk("  [app] round %d: sent id=0x%03x len=%d\n", round, id, sent);

		/* Give the loopback frame a moment to land in the queue. */
		k_msleep(50);

		uint32_t rid = 0;
		uint8_t rx[8] = { 0 };
		int n = svc_can_recv(&rid, rx, sizeof(rx));

		if (n >= 0) {
			printk("  [app] recv id=0x%03x len=%d data=", rid, n);
			for (int i = 0; i < n; i++) {
				printk("%02x ", rx[i]);
			}
			printk("\n");
		} else {
			printk("  [app] recv rc=%d\n", n);
		}

		k_msleep(300);
	}

	printk("  [app] done - used CAN send + receive\n");
}

int main(void)
{
	printk("\n*** more services 6: CAN bus (C6) ***\n");

	svc_can_install();

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
