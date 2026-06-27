/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/random/random.h>
#include <psa/crypto.h>
#include <string.h>
#include <errno.h>

#define PART_ID FIXED_PARTITION_ID(storage_partition)
#define XFER_MAX 128

static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));

/* A tiny in-RAM key/value store for the probe. */
struct kv_entry {
	char     key[16];
	uint32_t value;
	bool     used;
};
static struct kv_entry kv[8];

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

void z_impl_svc_heartbeat(void)
{
	app_platform_heartbeat();
}

uint32_t z_impl_svc_uptime(void)
{
	return (uint32_t)k_uptime_get();
}

int z_impl_svc_die_temp_milli(void)
{
	if (!device_is_ready(die_temp) || sensor_sample_fetch(die_temp) != 0) {
		return 0;
	}
	struct sensor_value v;

	sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v);
	return v.val1 * 1000 + v.val2 / 1000;
}

int z_impl_svc_random(uint8_t *buf, size_t len)
{
	if (len > XFER_MAX) {
		return -EINVAL;
	}
	sys_rand_get(buf, len);
	return 0;
}

int z_impl_svc_kv_set(const char *key, uint32_t value)
{
	struct kv_entry *free_slot = NULL;

	for (int i = 0; i < (int)ARRAY_SIZE(kv); i++) {
		if (kv[i].used && strcmp(kv[i].key, key) == 0) {
			kv[i].value = value;
			return 0;
		}
		if (!kv[i].used && free_slot == NULL) {
			free_slot = &kv[i];
		}
	}
	if (free_slot == NULL) {
		return -ENOSPC;
	}
	strncpy(free_slot->key, key, sizeof(free_slot->key) - 1);
	free_slot->value = value;
	free_slot->used = true;
	return 0;
}

int z_impl_svc_kv_get(const char *key, uint32_t *out)
{
	for (int i = 0; i < (int)ARRAY_SIZE(kv); i++) {
		if (kv[i].used && strcmp(kv[i].key, key) == 0) {
			*out = kv[i].value;
			return 0;
		}
	}
	return -ENOENT;
}

int z_impl_svc_sha(const uint8_t *in, size_t len, uint8_t *out32)
{
	if (len > XFER_MAX) {
		return -EINVAL;
	}
	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, len, out32, 32,
					   &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
}

int z_impl_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	const struct flash_area *fa;

	if (len > XFER_MAX || flash_area_open(PART_ID, &fa) != 0) {
		return -EIO;
	}
	int rc = flash_area_read(fa, off, buf, len);

	flash_area_close(fa);
	return rc;
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

static inline uint32_t z_vrfy_svc_uptime(void)
{
	return z_impl_svc_uptime();
}
#include <zephyr/syscalls/svc_uptime_mrsh.c>

static inline int z_vrfy_svc_die_temp_milli(void)
{
	return z_impl_svc_die_temp_milli();
}
#include <zephyr/syscalls/svc_die_temp_milli_mrsh.c>

static inline int z_vrfy_svc_random(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_random(buf, len);
}
#include <zephyr/syscalls/svc_random_mrsh.c>

static inline int z_vrfy_svc_kv_set(const char *key, uint32_t value)
{
	char buf[16];

	if (k_usermode_string_copy(buf, (char *)key, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_kv_set(buf, value);
}
#include <zephyr/syscalls/svc_kv_set_mrsh.c>

static inline int z_vrfy_svc_kv_get(const char *key, uint32_t *out)
{
	char buf[16];

	if (k_usermode_string_copy(buf, (char *)key, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_kv_get(buf, out);
}
#include <zephyr/syscalls/svc_kv_get_mrsh.c>

static inline int z_vrfy_svc_sha(const uint8_t *in, size_t len, uint8_t *out32)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(in, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out32, 32));
	return z_impl_svc_sha(in, len, out32);
}
#include <zephyr/syscalls/svc_sha_mrsh.c>

static inline int z_vrfy_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_flash_read(off, buf, len);
}
#include <zephyr/syscalls/svc_flash_read_mrsh.c>
#endif
