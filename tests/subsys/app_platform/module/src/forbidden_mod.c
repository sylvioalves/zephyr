/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A module that calls the implementation of a service directly instead of
 * the function exported to modules.
 */

#include <zephyr/app_platform/app_sdk.h>

void z_impl_app_svc_log(const char *msg);

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	z_impl_app_svc_log("bypassing the exported interface");
}
EXPORT_SYMBOL(app_main);
