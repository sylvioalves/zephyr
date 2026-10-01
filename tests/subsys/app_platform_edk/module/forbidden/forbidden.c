/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A module importing a kernel function outside the module interface, which the
 * kit refuses to sign.
 */

#include <zephyr/app_platform/app_sdk.h>

extern void k_sched_lock(void);

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	k_sched_lock();
}
EXPORT_SYMBOL(app_main);
