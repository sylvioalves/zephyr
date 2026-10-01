/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TESTS_APP_PLATFORM_THROUGHPUT_H_
#define TESTS_APP_PLATFORM_THROUGHPUT_H_

#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_memory/app_memdomain.h>

/* Launch a built-in application with the report partition in its domain. */
int tp_launch(const char *name, app_entry_t entry, app_caps_t caps);

/* Wait until no group carries @p name, up to @p ms milliseconds. */
bool tp_wait_gone(const char *name, int ms);

/* Sort @p n cycle counts in place. */
void tp_sort(uint32_t *v, size_t n);

#endif /* TESTS_APP_PLATFORM_THROUGHPUT_H_ */
