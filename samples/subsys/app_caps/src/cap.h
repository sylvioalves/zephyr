/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_CAPS_CAP_H_
#define APP_CAPS_CAP_H_

#include <zephyr/kernel.h>

/*
 * A capability layer for Zephyr - unforgeable, transferable, revocable tokens,
 * in the spirit of seL4/object-capability systems, replacing the access-control
 * list keyed by thread id.
 *
 * A capability token is a 64-bit opaque value: (slot << 32) | nonce. The kernel
 * holds the truth in a table; the app holds only the token. Three properties:
 *
 *   - UNFORGEABLE: the nonce is kernel-generated random. An app cannot fabricate
 *     a valid token because it cannot guess the nonce stored for a slot.
 *   - TRANSFERABLE: the holder may delegate a capability to another app; the
 *     kernel moves ownership, so the receiver gains it and the sender keeps it
 *     or not per the delegation. Here delegation MOVES the holder.
 *   - REVOCABLE: revoking clears the slot; every token referencing it dies.
 *
 * Invoking a capability checks: slot valid, nonce matches, and the caller is the
 * current holder. Any of forge / use-after-revoke / use-by-non-holder fails.
 */

typedef uint64_t cap_t;
#define CAP_NULL 0ULL

/* Resource a capability grants (toy resources for the demo). */
enum cap_resource {
	CAP_RES_SENSOR = 1,
	CAP_RES_LED,
	CAP_RES_STORAGE,
};

/* Supervisor: mint a capability for a resource, owned by @holder. */
cap_t cap_mint(enum cap_resource res, k_tid_t holder);

/* Invoke the capability (use the resource). Returns the resource id on success,
 * negative on any failure (forged, revoked, wrong holder).
 */
__syscall int cap_invoke(cap_t token);

/* Delegate: the calling holder transfers the capability to another thread. */
__syscall int cap_delegate(cap_t token, k_tid_t target);

/* Supervisor: revoke a capability; all tokens for it become dead. */
void cap_revoke(cap_t token);

#include <zephyr/syscalls/cap.h>

#endif /* APP_CAPS_CAP_H_ */
