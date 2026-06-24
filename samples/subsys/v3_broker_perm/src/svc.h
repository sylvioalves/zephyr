/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef V3_SVC_H_
#define V3_SVC_H_

#include <zephyr/kernel.h>

/*
 * Minimal service broker syscall. Access is gated on the caller holding
 * permission to a "service token" kobject (a semaphore). An app that was not
 * granted the token faults at the syscall boundary (only that app is killed);
 * a granted app passes through to the service action. This is exactly the
 * per-app service permission the full broker will use, isolated from any
 * particular driver's internals.
 */
__syscall int svc_request(struct k_sem *token);

#include <zephyr/syscalls/svc.h>

#endif /* V3_SVC_H_ */
