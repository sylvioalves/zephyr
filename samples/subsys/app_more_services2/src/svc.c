/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <psa/crypto.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>

#define KEY_MAX 32

void svc_init(void)
{
	psa_crypto_init();
	settings_subsys_init();
}

/* ---- crypto: SHA-256 ---- */
int z_impl_svc_sha256(const uint8_t *in, size_t in_len, uint8_t *out)
{
	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, in_len,
					   out, 32, &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
}

/* ---- key-value store over settings, namespaced under "app/" ---- */
struct kv_ctx {

	uint32_t    val;
	bool        found;
};

static int kv_direct_cb(const char *key, size_t len, settings_read_cb read_cb,
			void *cb_arg, void *param)
{
	struct kv_ctx *c = param;

	ARG_UNUSED(key);
	/* The subtree is the exact key, so any invocation is our value. */
	if (len == sizeof(uint32_t)) {
		read_cb(cb_arg, &c->val, sizeof(c->val));
		c->found = true;
	}
	return 0;
}

int z_impl_svc_kv_set(const char *key, uint32_t value)
{
	char path[KEY_MAX + 8];

	(void)snprintk(path, sizeof(path), "app/%s", key);
	return settings_save_one(path, &value, sizeof(value));
}

int z_impl_svc_kv_get(const char *key, uint32_t *out)
{
	char path[KEY_MAX + 8];

	(void)snprintk(path, sizeof(path), "app/%s", key);

	struct kv_ctx c = { .found = false };

	settings_load_subtree_direct(path, kv_direct_cb, &c);
	if (!c.found) {
		return -ENOENT;
	}
	*out = c.val;
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_sha256(const uint8_t *in, size_t in_len,
				    uint8_t *out)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(in, in_len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, 32));
	return z_impl_svc_sha256(in, in_len, out);
}
#include <zephyr/syscalls/svc_sha256_mrsh.c>

static inline int z_vrfy_svc_kv_set(const char *key, uint32_t value)
{
	char kbuf[KEY_MAX];

	if (k_usermode_string_copy(kbuf, (char *)key, sizeof(kbuf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_kv_set(kbuf, value);
}
#include <zephyr/syscalls/svc_kv_set_mrsh.c>

static inline int z_vrfy_svc_kv_get(const char *key, uint32_t *out)
{
	char kbuf[KEY_MAX];

	if (k_usermode_string_copy(kbuf, (char *)key, sizeof(kbuf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_kv_get(kbuf, out);
}
#include <zephyr/syscalls/svc_kv_get_mrsh.c>
#endif
