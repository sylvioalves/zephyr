/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app exchanging messages over IEEE 802.15.4 through a service from user
 * mode. The platform owns the radio interface, the net stack, and a UDP socket
 * to the peer; the app sends payloads and reads the echoes the peer returns,
 * never touching the radio or the socket directly. The peer is an esp32c5
 * running the echo server over 802.15.4.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	for (int round = 0; round < 5; round++) {
		char msg[32];
		int n = snprintk(msg, sizeof(msg), "hello-802154-%d", round);

		int sent = svc_radio_send((const uint8_t *)msg, n);

		printk("  [app] round %d: sent \"%s\" (%d bytes)\n", round, msg, sent);

		uint8_t in[64] = { 0 };
		int got = svc_radio_recv(in, sizeof(in) - 1, 1000);

		if (got > 0) {
			in[got] = '\0';
			printk("  [app] echo back (%d bytes): \"%s\"\n", got, in);
		} else {
			printk("  [app] no echo (rc=%d)\n", got);
		}

		k_msleep(500);
	}

	printk("  [app] done - messaged the peer over 802.15.4\n");
}

int main(void)
{
	printk("\n*** more services 14: IEEE 802.15.4 messaging (C6) ***\n");

	int rc = svc_radio_install();

	printk("  [platform] radio socket install rc=%d\n", rc);

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
