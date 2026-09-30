/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Kernel services exported to loadable application modules, in units that do
 * not depend on the kernel configuration.
 */

#include <zephyr/app_platform/app_abi.h>
#include <zephyr/kernel.h>

int32_t app_abi_sleep_ms_v1(int32_t ms)
{
	return k_msleep(ms);
}

int32_t app_abi_sleep_us_v1(int32_t us)
{
	return k_usleep(us);
}

int64_t app_abi_uptime_get_v1(void)
{
	return k_uptime_get();
}

void app_abi_yield_v1(void)
{
	k_yield();
}
