/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/app_platform/app_sdk.h>

void app_main(void *arg)
{
	char msg[64];

	ARG_UNUSED(arg);

	for (int i = 1; i <= 3; i++) {
		snprintf(msg, sizeof(msg), "hello: built outside the tree, run %d", i);
		app_svc_log(msg);
		app_sleep_ms(100);
	}
}
EXPORT_SYMBOL(app_main);
