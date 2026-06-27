/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIVENESS_SVC_H_
#define APP_LIVENESS_SVC_H_

#include <zephyr/kernel.h>

/* Minimal services for the liveness demo apps. */
__syscall void app_log(const char *msg);
__syscall void app_beat(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_LIVENESS_SVC_H_ */
