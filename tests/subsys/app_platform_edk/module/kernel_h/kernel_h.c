/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A module calling the kernel directly, which the kit cannot build. */

#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_sdk.h>

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	k_msleep(10);
}
EXPORT_SYMBOL(app_main);
