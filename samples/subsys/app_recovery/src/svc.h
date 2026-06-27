/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_RECOVERY_SVC_H_
#define APP_RECOVERY_SVC_H_

#include <zephyr/kernel.h>

/*
 * A checkpoint service: an app saves a small state value that the platform
 * holds outside the app's memory domain, so it survives the app crashing and
 * being restarted. The restarted instance reads it back and resumes.
 *
 *   svc_log            - log a line.
 *   svc_checkpoint_set - save this app's state value.
 *   svc_checkpoint_get - read this app's saved state value (0 if none).
 */

__syscall void svc_log(const char *msg);
__syscall void svc_checkpoint_set(uint32_t value);
__syscall uint32_t svc_checkpoint_get(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_RECOVERY_SVC_H_ */
