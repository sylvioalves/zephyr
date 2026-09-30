/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A module that calls the GPIO service, which the test firmware is built
 * without.
 */

#include <zephyr/app_platform/app_sdk.h>

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	(void)app_svc_gpio_count();
}
EXPORT_SYMBOL(app_main);
