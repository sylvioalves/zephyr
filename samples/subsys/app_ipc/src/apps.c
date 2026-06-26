/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two isolated apps that cooperate by message passing. The producer sends a
 * counter to the consumer through the broker; the consumer receives it and
 * sends an acknowledgment back. Neither can see the other's memory - only
 * copied bytes cross, routed by the platform.
 */

#include "svc.h"
#include "app_caps.h"

#include <zephyr/kernel.h>

/* producer: send a counter to the consumer, wait for its ack. */
void app_producer(void *arg)
{
	ARG_UNUSED(arg);

	for (uint8_t n = 1; n <= 5; n++) {
		uint8_t out = n;

		svc_ipc_send(APP_CONSUMER, &out, 1);
		svc_log("producer: sent a value to the consumer");

		int from = -1;
		uint8_t ack[8] = { 0 };
		int got = svc_ipc_recv(&from, ack, sizeof(ack), 1000);

		if (got > 0 && ack[0] == 'o') {
			svc_log("producer: got 'ok' ack back from the consumer");
		}
		k_msleep(500);
	}
	svc_log("producer: done");
}

/* consumer: receive values, log them, acknowledge each back to the sender. */
void app_consumer(void *arg)
{
	ARG_UNUSED(arg);

	for (;;) {
		int from = -1;
		uint8_t in[8] = { 0 };
		int got = svc_ipc_recv(&from, in, sizeof(in), 2000);

		if (got > 0) {
			svc_log("consumer: received a value, acking back");

			uint8_t ack[2] = { 'o', 'k' };

			svc_ipc_send(from, ack, sizeof(ack));
		} else {
			svc_log("consumer: idle (no message)");
		}
	}
}
