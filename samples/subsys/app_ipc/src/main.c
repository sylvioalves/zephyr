/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Inter-application messaging on the application platform. Two isolated apps -
 * a producer and a consumer - cannot touch each other's memory, so they
 * cooperate through a capability-gated broker: the producer sends values to the
 * consumer, the consumer acknowledges them back. Only copied bytes cross the
 * isolation boundary, and only because both apps hold the IPC capability.
 */

#include "app_caps.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern void svc_ipc_init(void);
extern void app_producer(void *arg);
extern void app_consumer(void *arg);

int main(void)
{
	printk("\n*** app platform: inter-app messaging (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	svc_ipc_init();

	/* Launch order fixes the group ids the apps message by. */
	const struct app_def producer = {
		.name = "producer", .source = APP_BUILTIN,
		.entry = app_producer, .caps = CAP_IPC,
	};
	const struct app_def consumer = {
		.name = "consumer", .source = APP_BUILTIN,
		.entry = app_consumer, .caps = CAP_IPC,
	};

	app_platform_launch(&producer);   /* group APP_PRODUCER (0) */
	app_platform_launch(&consumer);   /* group APP_CONSUMER (1) */

	k_msleep(4000);

	printk("\n*** done: two isolated apps cooperated by message passing ***\n");
	return 0;
}
