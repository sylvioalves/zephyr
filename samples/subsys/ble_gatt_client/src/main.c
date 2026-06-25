/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * BLE GATT client service (Tier 2): an isolated user-mode app asks the broker
 * for "heart rate". The broker scans, connects to a Heart Rate peer, discovers
 * the characteristic, and subscribes; notifications are delivered to the app
 * through its event queue. The app never touches the controller, the
 * connection, or the GATT layer - it just consumes heart-rate values.
 *
 * Pair with any Heart Rate peer: a fitness strap, or a phone running a GATT
 * server app (e.g. nRF Connect) advertising the Heart Rate Service.
 */

#include "gatt_svc.h"
#include "gatt_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

K_MSGQ_DEFINE(app_q, sizeof(struct gatt_notification), 8, 4);

static const char *state_name(uint8_t s)
{
	switch (s) {
	case GATT_SCANNING:     return "scanning";
	case GATT_CONNECTED:    return "connected";
	case GATT_SUBSCRIBED:   return "subscribed";
	case GATT_DISCONNECTED: return "disconnected";
	default:                return "?";
	}
}

static void hr_app(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	printk("  [hr-app] requesting heart-rate service\n");
	svc_ble_hrs_start();

	for (;;) {
		struct gatt_notification ev;
		int rc = svc_ble_next_event(&ev, 5000);

		if (rc != 0) {
			printk("  [hr-app] waiting...\n");
			continue;
		}

		if (ev.handle == 0xFFFF) {
			printk("  [hr-app] state: %s\n", state_name(ev.data[0]));
			continue;
		}

		/* Heart Rate Measurement: byte 0 = flags, byte 1 = bpm (8-bit). */
		uint8_t bpm = (ev.len >= 2) ? ev.data[1] : 0;

		printk("  [hr-app] heart rate notification: %u bpm (len %u)\n",
		       bpm, ev.len);
	}
}

int main(void)
{
	printk("\n*** BLE GATT client service: app subscribes to heart rate (C6) ***\n");

	if (gatt_broker_init() != 0) {
		printk("gatt_broker_init failed\n");
		return 0;
	}

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, hr_app,
				    NULL, NULL, NULL, APP_PRIO, K_USER,
				    K_FOREVER);

	k_thread_access_grant(t, &app_q);
	gatt_svc_register(t, &app_q);
	k_thread_start(t);
	return 0;
}
