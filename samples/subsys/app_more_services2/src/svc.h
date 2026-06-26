/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES2_SVC_H_
#define APP_MORE_SERVICES2_SVC_H_

#include <zephyr/kernel.h>

/*
 * A second batch of services, touching subsystems not covered elsewhere:
 *
 *   crypto (PSA)   - svc_sha256: hash a buffer; an open, stateless service.
 *   key-value store - svc_kv_set / svc_kv_get over the settings subsystem; a
 *                     per-app keyspace persisted across reboots (namespaced).
 *
 * An app stores a value, reads it back, and hashes data, all through service
 * syscalls; the keyed data survives a reboot.
 */

/* Open: SHA-256 of @in (len @in_len) into the 32-byte @out. */
__syscall int svc_sha256(const uint8_t *in, size_t in_len, uint8_t *out);

/* Namespaced KV: persist a small value under @key (within the app's keyspace). */
__syscall int svc_kv_set(const char *key, uint32_t value);
/* Read it back; writes the value to @out. Returns 0 or -ENOENT. */
__syscall int svc_kv_get(const char *key, uint32_t *out);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES2_SVC_H_ */
