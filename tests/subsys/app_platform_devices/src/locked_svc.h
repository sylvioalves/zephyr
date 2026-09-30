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

#define TEST_HOLD_OWN  0 /* take the lock and keep it until released */
#define TEST_HOLD_WAIT 1 /* wait for the lock with no timeout */
#define TEST_HOLD_USE  2 /* take the lock and give it back */

/* A service whose lock one application keeps while others wait for it. */
__syscall int test_svc_hold(int mode);

void test_svc_hold_release(void);
bool test_svc_hold_taken(void);

/* Make the next external RAM mapping fault while it holds the service lock. */
void test_psram_fault_arm(void);
bool test_psram_fault_armed(void);

#include <zephyr/syscalls/locked_svc.h>

#endif /* LOCKED_SVC_H_ */
