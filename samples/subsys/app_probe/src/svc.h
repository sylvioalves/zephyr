/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PROBE_SVC_H_
#define APP_PROBE_SVC_H_

#include <zephyr/kernel.h>

/*
 * A spread of services chosen to exercise every kind of syscall boundary from a
 * loadable app: scalar return, buffer write to the app, string read from the
 * app, buffer in and buffer out, and a peripheral read. The point is to find out
 * which service shapes work unchanged from an llext module and which need care.
 */
__syscall void svc_log(const char *msg);
__syscall void svc_heartbeat(void);

/* scalar return, no buffer */
__syscall uint32_t svc_uptime(void);

/* sensor scalar (control - already known to work from llext) */
__syscall int svc_die_temp_milli(void);

/* write a buffer back to the app */
__syscall int svc_random(uint8_t *buf, size_t len);

/* read a string from the app, plus a scalar */
__syscall int svc_kv_set(const char *key, uint32_t value);

/* read a string from the app, write a scalar back */
__syscall int svc_kv_get(const char *key, uint32_t *out);

/* read a buffer from the app and write a buffer back (PSA SHA-256) */
__syscall int svc_sha(const uint8_t *in, size_t len, uint8_t *out32);

/* peripheral read into an app buffer (raw flash partition) */
__syscall int svc_flash_read(uint32_t off, uint8_t *buf, size_t len);

#include <zephyr/syscalls/svc.h>

#endif /* APP_PROBE_SVC_H_ */
