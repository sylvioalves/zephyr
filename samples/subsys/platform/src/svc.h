/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLATFORM_SVC_H_
#define PLATFORM_SVC_H_

#include <zephyr/kernel.h>

/* The platform service syscalls apps call from user mode. */
__syscall int  svc_sensor_read(int32_t *milli_c);
__syscall void svc_log(const char *msg);
__syscall void svc_heartbeat(void);

#include <zephyr/syscalls/svc.h>

#endif /* PLATFORM_SVC_H_ */
