/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <profile_api.h>

#if !defined(PROFILE_EDK_CONFIG) || defined(CONFIG_MAIN_STACK_SIZE)
#error "The EDK configuration header is not the one of the application"
#endif

#if __has_include(<zephyr/kernel.h>)
#error "The EDK holds headers that were not selected"
#endif

int start(int value)
{
	return profile_scale(value) + 1;
}
EXPORT_SYMBOL(start);
