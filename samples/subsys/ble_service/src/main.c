/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * BLE scan service: the broker owns the single controller and runs one shared
 * scan; discovered devices are fanned out to every app. Two isolated user-mode
 * apps both "scan" in parallel - each receives the same stream of devices on its
 * own event queue - exactly how a phone delivers one radio scan to many apps.
 */

#include "ble_svc.h"
#include "ble_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define APP_PRIO   5
#define QDEPTH     8

K_THREAD_STACK_DEFINE(stack_a, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_b, STACK_SIZE);
static struct k_thread th_a, th_b;

/* One event queue per app (the async-event channel into each isolated app). */
K_MSGQ_DEFINE(q_a, sizeof(struct ble_scan_event), QDEPTH, 4);
K_MSGQ_DEFINE(q_b, sizeof(struct ble_scan_event), QDEPTH, 4);

/* A BLE app: subscribe to the scan service, then consume devices in user mode. */
static void scanner_app(void *name_p, void *p2, void *p3)
{
	ARG_UNUSED(p2); ARG_UNUSED(p3);
	const char *name = name_p;

	int sub = svc_ble_scan_start();

	printk("  [%s] subscribed to BLE scan (sub %d)\n", name, sub);

	int seen = 0;

	for (;;) {
		struct ble_scan_event ev;
		int rc = svc_ble_next_device(&ev, 3000);

		if (rc == 0) {
			printk("  [%s] device %s rssi %d type %u adlen %u (#%d)\n",
			       name, ev.addr, ev.rssi, ev.adv_type, ev.ad_len,
			       ++seen);
		} else {
			printk("  [%s] (no device in 3s)\n", name);
		}
	}
}

int main(void)
{
	printk("\n*** BLE scan service: one radio, two apps scanning in parallel (C6) ***\n");

	if (ble_broker_init() != 0) {
		printk("ble_broker_init failed\n");
		return 0;
	}

	k_tid_t ta = k_thread_create(&th_a, stack_a, STACK_SIZE, scanner_app,
				     "app-A", NULL, NULL, APP_PRIO, K_USER,
				     K_FOREVER);
	k_tid_t tb = k_thread_create(&th_b, stack_b, STACK_SIZE, scanner_app,
				     "app-B", NULL, NULL, APP_PRIO, K_USER,
				     K_FOREVER);

	/* Grant each app its own event queue and register it with the broker. */
	k_thread_access_grant(ta, &q_a);
	k_thread_access_grant(tb, &q_b);
	ble_svc_register(ta, &q_a);
	ble_svc_register(tb, &q_b);

	k_thread_start(ta);
	k_thread_start(tb);
	return 0;
}
