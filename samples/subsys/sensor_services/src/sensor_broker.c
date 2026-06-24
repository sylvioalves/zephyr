/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sensor_broker.h"
#include "sensor_catalog.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>

int z_impl_svc_sensor_count(void)
{
	return sensor_catalog_count();
}

int z_impl_svc_sensor_info(int idx, char *name, size_t name_sz,
			   char *unit, size_t unit_sz, bool *present)
{
	struct sensor_svc s;
	int rc = sensor_catalog_get(idx, &s);

	if (rc != 0) {
		return rc;
	}

	strncpy(name, s.name, name_sz - 1);
	name[name_sz - 1] = '\0';
	strncpy(unit, s.unit, unit_sz - 1);
	unit[unit_sz - 1] = '\0';
	*present = s.present;
	return 0;
}

int z_impl_svc_sensor_read(int idx, int32_t *milli_value)
{
	return sensor_catalog_read(idx, milli_value);
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_svc_sensor_count(void)
{
	return z_impl_svc_sensor_count();
}
#include <zephyr/syscalls/svc_sensor_count_mrsh.c>

static inline int z_vrfy_svc_sensor_info(int idx, char *name, size_t name_sz,
					 char *unit, size_t unit_sz,
					 bool *present)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(name, name_sz));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(unit, unit_sz));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(present, sizeof(*present)));
	return z_impl_svc_sensor_info(idx, name, name_sz, unit, unit_sz, present);
}
#include <zephyr/syscalls/svc_sensor_info_mrsh.c>

static inline int z_vrfy_svc_sensor_read(int idx, int32_t *milli_value)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(milli_value, sizeof(*milli_value)));
	return z_impl_svc_sensor_read(idx, milli_value);
}
#include <zephyr/syscalls/svc_sensor_read_mrsh.c>

#endif /* CONFIG_USERSPACE */
