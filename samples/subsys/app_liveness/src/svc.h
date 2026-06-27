/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIVENESS_SVC_H_
#define APP_LIVENESS_SVC_H_

#include <zephyr/kernel.h>

/* Minimal services for the liveness demo apps. */
__syscall void svc_log(const char *msg);
__syscall void svc_heartbeat(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_LIVENESS_SVC_H_ */
