/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CONTROL_SVC_H_
#define APP_CONTROL_SVC_H_

#include <zephyr/kernel.h>

/*
 * Runtime lifecycle control: a privileged "manager" app drives the platform to
 * start and stop other apps while the system runs. The control operations are
 * brokered and gated by a control capability, so only a trusted app can manage
 * the others - an app store / launcher front-end built as just another app.
 *
 *   svc_log        - log a line.
 *   svc_ctrl_start - start a worker by catalog name; returns its group id.
 *   svc_ctrl_stop  - stop a running group by id. Needs CAP_CONTROL.
 */

__syscall void svc_log(const char *msg);
__syscall void svc_heartbeat(void);

/* Scoped (CAP_CONTROL): start a catalog worker by name. Returns group id. */
__syscall int svc_ctrl_start(const char *name);

/* Scoped (CAP_CONTROL): stop the running group @id. */
__syscall int svc_ctrl_stop(int id);

#include <zephyr/syscalls/svc.h>

#endif /* APP_CONTROL_SVC_H_ */
