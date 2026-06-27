/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_INSPECT_SVC_H_
#define APP_INSPECT_SVC_H_

#include <zephyr/kernel.h>

/* User-mode-safe snapshot of one app group (name is a copy, not a pointer). */
struct svc_app_info {
	char     name[16];
	bool     active;
	bool     monitored;
	bool     faulted;
	uint32_t stack_size;
	uint32_t stack_used;
	uint64_t cpu_cycles;
	uint64_t cpu_total;
};

/* Services for the inspect demo. */
__syscall void svc_log(const char *msg);

/* Number of app slots. */
__syscall int svc_app_count(void);

/*
 * Read group @id into @out. Requires the inspect capability; an app without it
 * gets -EPERM, so one app cannot observe the others unless granted.
 */
__syscall int svc_app_info(int id, struct svc_app_info *out);

#include <zephyr/syscalls/svc.h>

#endif /* APP_INSPECT_SVC_H_ */
