/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fs_svc.h"
#include "fs_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>

#define NAME_MAX 48

int z_impl_app_file_open(const char *path, int create)
{
	return fs_broker_open(path, create != 0);
}

int z_impl_app_file_write(int handle, const char *data, size_t len)
{
	return fs_broker_write(handle, data, len);
}

int z_impl_app_file_read(int handle, char *buf, size_t len)
{
	return fs_broker_read(handle, buf, len);
}

int z_impl_app_file_close(int handle)
{
	return fs_broker_close(handle);
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_app_file_open(const char *path, int create)
{
	char copy[NAME_MAX];

	if (k_usermode_string_copy(copy, (char *)path, sizeof(copy)) != 0) {
		return -EFAULT;
	}
	return z_impl_app_file_open(copy, create);
}
#include <zephyr/syscalls/app_file_open_mrsh.c>

static inline int z_vrfy_app_file_write(int handle, const char *data, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(data, len));
	return z_impl_app_file_write(handle, data, len);
}
#include <zephyr/syscalls/app_file_write_mrsh.c>

static inline int z_vrfy_app_file_read(int handle, char *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_app_file_read(handle, buf, len);
}
#include <zephyr/syscalls/app_file_read_mrsh.c>

static inline int z_vrfy_app_file_close(int handle)
{
	return z_impl_app_file_close(handle);
}
#include <zephyr/syscalls/app_file_close_mrsh.c>

#endif /* CONFIG_USERSPACE */
