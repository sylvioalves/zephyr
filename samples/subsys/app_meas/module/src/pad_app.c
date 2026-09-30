/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/app_platform/app_sdk.h>

#if PAD_BYTES > 0
static const uint8_t pad[PAD_BYTES] = { [0] = 1, [PAD_BYTES - 1] = 2 };
#endif

void app_main(void *arg)
{
	ARG_UNUSED(arg);
#if PAD_BYTES > 0
	volatile uint8_t sink = pad[0] + pad[PAD_BYTES - 1];

	ARG_UNUSED(sink);
#endif
	for (;;) {
		app_svc_log("pad app alive");
		app_sleep_ms(1000);
	}
}
EXPORT_SYMBOL(app_main);
