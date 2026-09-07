/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <errno.h>

#define XFER_MAX 256

/* The platform owns the storage partition; the app is confined to it. */
#define PART_ID PARTITION_ID(storage_partition)

static const struct flash_area *fa;

int svc_flash_install(void)
{
	return flash_area_open(PART_ID, &fa);
}

/* Bounds: offset + len must stay inside the partition. */
static bool in_part(uint32_t off, size_t len)
{
	return fa != NULL && (uint64_t)off + len <= (uint64_t)fa->fa_size;
}

/* For read/write the payload also cannot exceed the copy buffer cap. */
static bool in_bounds(uint32_t off, size_t len)
{
	return in_part(off, len) && len <= XFER_MAX;
}

int z_impl_svc_flash_erase(uint32_t off, size_t len)
{
	/* Erase spans whole sectors, so it is bounded only by the partition. */
	if (!in_part(off, len)) {
		return -EINVAL;
	}
	return flash_area_erase(fa, off, len);
}

int z_impl_svc_flash_write(uint32_t off, const uint8_t *buf, size_t len)
{
	if (!in_bounds(off, len)) {
		return -EINVAL;
	}
	int rc = flash_area_write(fa, off, buf, len);

	return (rc == 0) ? (int)len : rc;
}

int z_impl_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	if (!in_bounds(off, len)) {
		return -EINVAL;
	}
	int rc = flash_area_read(fa, off, buf, len);

	return (rc == 0) ? (int)len : rc;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_flash_erase(uint32_t off, size_t len)
{
	return z_impl_svc_flash_erase(off, len);
}
#include <zephyr/syscalls/svc_flash_erase_mrsh.c>

static inline int z_vrfy_svc_flash_write(uint32_t off, const uint8_t *buf, size_t len)
{
	if (len > XFER_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_flash_write(off, buf, len);
}
#include <zephyr/syscalls/svc_flash_write_mrsh.c>

static inline int z_vrfy_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	if (len > XFER_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_flash_read(off, buf, len);
}
#include <zephyr/syscalls/svc_flash_read_mrsh.c>
#endif
