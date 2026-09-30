/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App 2 (bridge): subscribe to the temperature topic at the broker, receive
 * each die-temp value over MQTT, and forward it to app 3 over the platform's
 * inter-application IPC. It bridges the network and the in-device message bus
 * without sharing memory with either neighbour.
 */

#include <zephyr/app_platform/app_sdk.h>

static size_t z_strlen(const char *s)
{
	size_t n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	app_svc_log("bridge: waiting for the shared network link");
	if (app_svc_net_ready() != 0) {
		app_svc_log("bridge: network not ready");
		return;
	}
	/* Subscribe to the same topic the publisher uses. */
	if (app_svc_mqtt_open("zephyr-trio-bridge", "zephyr/trio/unused",
			      "zephyr/trio/temp") != 0) {
		app_svc_log("bridge: MQTT session failed");
		return;
	}
	app_svc_log("bridge: receiving die temp over MQTT, forwarding via IPC");

	for (;;) {
		char temp[16];
		int n = app_svc_mqtt_recv(temp, sizeof(temp), 10000);

		if (n <= 0) {
			continue;
		}

		char line[48];
		int i = 0;
		const char *p = "bridge: got ";

		while (*p) {
			line[i++] = *p++;
		}
		for (int j = 0; j < n && j < (int)sizeof(temp); j++) {
			line[i++] = temp[j];
		}
		line[i] = '\0';
		app_svc_log(line);

		/* Forward to app 3 by name, over isolation-safe IPC. */
		int sr = app_svc_ipc_send_to("printer", temp, z_strlen(temp) + 1);

		app_svc_log(sr == 0 ? "bridge: forwarded to printer"
				    : "bridge: forward FAILED");
	}
}
EXPORT_SYMBOL(app_main);
