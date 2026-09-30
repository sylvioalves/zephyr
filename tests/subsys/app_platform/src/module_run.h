/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PLATFORM_TEST_MODULE_RUN_H_
#define APP_PLATFORM_TEST_MODULE_RUN_H_

#include <stdbool.h>

/* Record whether the application named @p name faults from now on. */
void module_watch(const char *name);

/* Whether the watched application faulted since module_watch(). */
bool module_faulted(void);

/* Wait up to @p ms for the watched application to end. Returns 0 when it
 * returned, -EFAULT when it faulted, and -ETIMEDOUT when it was still running,
 * in which case it is removed.
 */
int module_wait_end(int id, int ms);

#endif /* APP_PLATFORM_TEST_MODULE_RUN_H_ */
