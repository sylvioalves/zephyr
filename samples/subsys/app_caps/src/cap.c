/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/internal/syscall_handler.h>

#define CAP_SLOTS 16

/* The kernel-held capability table - the single source of truth. */
struct cap_slot {
	bool              valid;
	enum cap_resource res;
	k_tid_t           holder;
	uint32_t          nonce;   /* kernel-random; the app never sees the table */
};

static struct cap_slot table[CAP_SLOTS];

static inline cap_t make_token(uint32_t slot, uint32_t nonce)
{
	return ((uint64_t)slot << 32) | nonce;
}

static inline uint32_t token_slot(cap_t t)
{
	return (uint32_t)(t >> 32);
}

static inline uint32_t token_nonce(cap_t t)
{
	return (uint32_t)(t & 0xFFFFFFFF);
}

cap_t cap_mint(enum cap_resource res, k_tid_t holder)
{
	for (uint32_t i = 0; i < CAP_SLOTS; i++) {
		if (!table[i].valid) {
			uint32_t nonce;

			sys_rand_get(&nonce, sizeof(nonce));
			if (nonce == 0) {
				nonce = 1;   /* never zero */
			}
			table[i].valid = true;
			table[i].res = res;
			table[i].holder = holder;
			table[i].nonce = nonce;
			return make_token(i, nonce);
		}
	}
	return CAP_NULL;
}

/* Validate a token: in-range, valid, nonce matches, caller is holder. */
static struct cap_slot *resolve(cap_t token, k_tid_t caller, const char **why)
{
	uint32_t slot = token_slot(token);

	if (slot >= CAP_SLOTS) {
		*why = "bad slot";
		return NULL;
	}
	struct cap_slot *s = &table[slot];

	if (!s->valid) {
		*why = "revoked";
		return NULL;
	}
	if (s->nonce != token_nonce(token)) {
		*why = "forged (nonce mismatch)";
		return NULL;
	}
	if (s->holder != caller) {
		*why = "not the holder";
		return NULL;
	}
	return s;
}

int z_impl_cap_invoke(cap_t token)
{
	const char *why = "";
	struct cap_slot *s = resolve(token, k_current_get(), &why);

	if (s == NULL) {
		printk("    [cap] invoke DENIED: %s\n", why);
		return -EPERM;
	}
	printk("    [cap] invoke OK: resource %d used\n", s->res);
	return (int)s->res;
}

int z_impl_cap_delegate(cap_t token, k_tid_t target)
{
	const char *why = "";
	struct cap_slot *s = resolve(token, k_current_get(), &why);

	if (s == NULL) {
		printk("    [cap] delegate DENIED: %s\n", why);
		return -EPERM;
	}
	/* Move ownership: the receiver gains it, the sender loses it. */
	s->holder = target;
	printk("    [cap] delegated resource %d to thread %p\n", s->res,
	       (void *)target);
	return 0;
}

void cap_revoke(cap_t token)
{
	uint32_t slot = token_slot(token);

	if (slot < CAP_SLOTS && table[slot].valid &&
	    table[slot].nonce == token_nonce(token)) {
		table[slot].valid = false;
		table[slot].nonce = 0;
		printk("    [cap] revoked resource %d (all tokens now dead)\n",
		       table[slot].res);
	}
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_cap_invoke(cap_t token)
{
	return z_impl_cap_invoke(token);
}
#include <zephyr/syscalls/cap_invoke_mrsh.c>

static inline int z_vrfy_cap_delegate(cap_t token, k_tid_t target)
{
	/* target is an opaque thread handle; the impl validates the caller is
	 * the holder, so a forged target only hands a cap the caller already
	 * owns to some thread - no escalation.
	 */
	return z_impl_cap_delegate(token, target);
}
#include <zephyr/syscalls/cap_delegate_mrsh.c>
#endif
