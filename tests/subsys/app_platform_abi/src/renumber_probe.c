/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>

#include "renumber_probe.h"

int z_impl_aaa_renumber_probe(int value)
{
	return value + 1;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_aaa_renumber_probe(int value)
{
	return z_impl_aaa_renumber_probe(value);
}
#include <zephyr/syscalls/aaa_renumber_probe_mrsh.c>
#endif
