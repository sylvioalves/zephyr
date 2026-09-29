/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App 3 (printer): register a name so others can address it, then receive the
 * forwarded die-temp values over inter-application IPC and print them. It never
 * touches the network - it only consumes the in-device message bus.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	if (app_svc_ipc_register("printer") != 0) {
		app_svc_log("printer: could not register name");
		return;
	}
	app_svc_log("printer: registered, waiting for temps over IPC");

	for (;;) {
		char temp[16];
		int n = app_svc_ipc_recv(temp, sizeof(temp), 10000);

		if (n <= 0) {
			continue;
		}

		char line[64];
		int i = 0;
		const char *p = "printer: received temp ";

		while (*p) {
			line[i++] = *p++;
		}
		for (int j = 0; j < n && j < (int)sizeof(temp) && temp[j]; j++) {
			line[i++] = temp[j];
		}
		line[i++] = ' ';
		line[i++] = 'C';
		line[i] = '\0';
		app_svc_log(line);
	}
}
EXPORT_SYMBOL(app_main);
