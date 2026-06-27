/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The shared service library. Each service runs in the supervisor, enforces its
 * capability bit at the syscall boundary, and is safe to call concurrently from
 * several isolated applications. This first group (log, crypto, random, time) is
 * stateless beyond the cap check, so concurrency needs no locking.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/random/random.h>
#ifdef CONFIG_PSA_CRYPTO
#include <psa/crypto.h>
#endif
#include <errno.h>

#define SVC_XFER_MAX 256

/* True when the calling application holds every bit in @mask. */
static inline bool app_has_cap(uint32_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

void z_impl_svc_heartbeat(void)
{
	app_platform_heartbeat();
}

int z_impl_svc_sha256(const uint8_t *in, size_t len, uint8_t *out32)
{
	if (!app_has_cap(APP_CAP_CRYPTO)) {
		return -EPERM;
	}
	if (len > SVC_XFER_MAX) {
		return -EINVAL;
	}
#ifdef CONFIG_PSA_CRYPTO
	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, len, out32, 32,
					   &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
#else
	ARG_UNUSED(out32);
	return -ENOTSUP;
#endif
}

int z_impl_svc_random(uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_RANDOM)) {
		return -EPERM;
	}
	if (len > SVC_XFER_MAX) {
		return -EINVAL;
	}
	sys_rand_get(buf, len);
	return 0;
}

uint32_t z_impl_svc_uptime_ms(void)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return 0;
	}
	return (uint32_t)k_uptime_get();
}

uint32_t z_impl_svc_cycles(void)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return 0;
	}
	return k_cycle_get_32();
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_svc_log(const char *msg)
{
	char buf[64];

	if (k_usermode_string_copy(buf, (char *)msg, sizeof(buf)) == 0) {
		z_impl_svc_log(buf);
	}
}
#include <zephyr/syscalls/svc_log_mrsh.c>

static inline void z_vrfy_svc_heartbeat(void)
{
	z_impl_svc_heartbeat();
}
#include <zephyr/syscalls/svc_heartbeat_mrsh.c>

static inline int z_vrfy_svc_sha256(const uint8_t *in, size_t len,
				    uint8_t *out32)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(in, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out32, 32));
	return z_impl_svc_sha256(in, len, out32);
}
#include <zephyr/syscalls/svc_sha256_mrsh.c>

static inline int z_vrfy_svc_random(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_random(buf, len);
}
#include <zephyr/syscalls/svc_random_mrsh.c>

static inline uint32_t z_vrfy_svc_uptime_ms(void)
{
	return z_impl_svc_uptime_ms();
}
#include <zephyr/syscalls/svc_uptime_ms_mrsh.c>

static inline uint32_t z_vrfy_svc_cycles(void)
{
	return z_impl_svc_cycles();
}
#include <zephyr/syscalls/svc_cycles_mrsh.c>
#endif
