/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * One source built three times as three isolated apps that talk to each other
 * through the platform's messaging services. The producer sends a directed
 * message to the consumer by name and publishes a reading to a topic; the
 * consumer receives the directed message; the logger, subscribed to the topic,
 * receives the broadcast. All three run in their own memory domains and exchange
 * only copied bytes - no shared memory.
 */

#include <zephyr/app_platform/app_services.h>

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

#ifndef ROLE
#define ROLE 0
#endif

#define ROLE_PRODUCER 1
#define ROLE_CONSUMER 2
#define ROLE_LOGGER   3

void app_main(void *arg)
{
	ARG_UNUSED(arg);

#if ROLE == ROLE_PRODUCER
	svc_ipc_register("producer");
	/* Give the others a moment to register and subscribe. */
	k_msleep(500);

	svc_log("producer: sending to consumer by name");
	int rc = svc_ipc_send_to("consumer", "hello-consumer", 15);

	svc_log(rc == 0 ? "producer: directed send ok" : "producer: send FAILED");

	svc_log("producer: publishing to topic sensors/temp");
	int n = svc_topic_publish("sensors/temp", "23.4", 5);

	svc_log(n >= 1 ? "producer: published to subscriber(s)"
		       : "producer: no subscribers");

#elif ROLE == ROLE_CONSUMER
	svc_ipc_register("consumer");
	svc_log("consumer: waiting for a directed message");

	uint8_t buf[32] = {0};
	int len = svc_ipc_recv(buf, sizeof(buf), 3000);

	if (len > 0 && buf[0] == 'h') {
		svc_log("consumer: GOT directed message from producer");
	} else {
		svc_log("consumer: no message");
	}

#elif ROLE == ROLE_LOGGER
	svc_ipc_register("logger");
	svc_topic_subscribe("sensors/temp");
	svc_log("logger: subscribed to sensors/temp, waiting");

	uint8_t buf[32] = {0};
	int len = svc_ipc_recv(buf, sizeof(buf), 3000);

	if (len > 0 && buf[0] == '2') {
		svc_log("logger: GOT topic broadcast from producer");
	} else {
		svc_log("logger: no broadcast");
	}
#endif

	for (;;) {
		svc_heartbeat();
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
