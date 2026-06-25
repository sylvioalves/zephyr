/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Multi-app BLE: several isolated apps use one shared radio at the same time.
 * App A continuously scans; App B connects to a Heart Rate peer and receives
 * live notifications - both concurrently, each on its own event queue. Adding
 * App C (a connection to a second peripheral) is one more app block; the broker
 * already supports up to BT_MAX_CONN concurrent connections.
 */

#include "ble_svc.h"
#include "ble_mux.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define APP_PRIO   5
#define QDEPTH     8

K_THREAD_STACK_DEFINE(stack_a, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_b, STACK_SIZE);
static struct k_thread th_a, th_b;

K_MSGQ_DEFINE(q_a, sizeof(struct ble_event), QDEPTH, 4);
K_MSGQ_DEFINE(q_b, sizeof(struct ble_event), QDEPTH, 4);

static const char *kind_name(uint8_t k)
{
	switch (k) {
	case BLE_EVT_SCAN:         return "scan";
	case BLE_EVT_CONNECTED:    return "connected";
	case BLE_EVT_SUBSCRIBED:   return "subscribed";
	case BLE_EVT_NOTIFY:       return "notify";
	case BLE_EVT_DISCONNECTED: return "disconnected";
	default:                   return "?";
	}
}

/* App A: a scanner. Joins the shared scan and reports discovered devices. */
static void scanner_app(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	svc_ble_scan();
	printk("  [A:scanner] scanning\n");

	int n = 0;

	for (;;) {
		struct ble_event ev;

		if (svc_ble_next(&ev, 4000) != 0) {
			continue;
		}
		if (ev.kind == BLE_EVT_SCAN) {
			printk("  [A:scanner] device %s rssi %d (#%d)\n",
			       ev.addr, ev.rssi, ++n);
		}
	}
}

/* App B: connects to a Heart Rate peer and prints live bpm. */
static void hr_app(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	svc_ble_connect();
	printk("  [B:heart-rate] connecting to a Heart Rate peer\n");

	for (;;) {
		struct ble_event ev;

		if (svc_ble_next(&ev, 4000) != 0) {
			continue;
		}
		switch (ev.kind) {
		case BLE_EVT_CONNECTED:
		case BLE_EVT_SUBSCRIBED:
		case BLE_EVT_DISCONNECTED:
			printk("  [B:heart-rate] %s %s\n", kind_name(ev.kind),
			       ev.addr);
			break;
		case BLE_EVT_NOTIFY:
			printk("  [B:heart-rate] %u bpm\n",
			       (ev.len >= 2) ? ev.val[1] : 0);
			break;
		default:
			break;
		}
	}
}

int main(void)
{
	printk("\n*** multi-app BLE: scan + connect concurrently on one radio (C6) ***\n");

	if (ble_mux_init() != 0) {
		printk("ble_mux_init failed\n");
		return 0;
	}

	k_tid_t ta = k_thread_create(&th_a, stack_a, STACK_SIZE, scanner_app,
				     NULL, NULL, NULL, APP_PRIO, K_USER,
				     K_FOREVER);
	k_tid_t tb = k_thread_create(&th_b, stack_b, STACK_SIZE, hr_app,
				     NULL, NULL, NULL, APP_PRIO, K_USER,
				     K_FOREVER);

	k_thread_access_grant(ta, &q_a);
	k_thread_access_grant(tb, &q_b);
	ble_svc_register(ta, &q_a);
	ble_svc_register(tb, &q_b);

	k_thread_start(ta);
	k_thread_start(tb);
	return 0;
}
