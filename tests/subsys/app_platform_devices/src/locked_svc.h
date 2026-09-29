/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LOCKED_SVC_H_
#define LOCKED_SVC_H_

#include <zephyr/kernel.h>

/* A service that takes a service lock and, when asked, faults while holding it. */
__syscall int test_svc_locked(int fault);

int test_svc_locked_hook_register(void);
int test_svc_locked_hook_runs(void);

#include <zephyr/syscalls/locked_svc.h>

#endif /* LOCKED_SVC_H_ */
