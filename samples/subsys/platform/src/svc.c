/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"
#include "platform.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/internal/syscall_handler.h>

extern int manager_id_of_current(void);
extern void manager_app_feed(void);

static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));

int z_impl_svc_sensor_read(int32_t *milli_c)
{
	if (!device_is_ready(die_temp) || sensor_sample_fetch(die_temp) != 0) {
		return -EIO;
	}
	struct sensor_value v;

	if (sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v) != 0) {
		return -EIO;
	}
	*milli_c = v.val1 * 1000 + v.val2 / 1000;
	return 0;
}

void z_impl_svc_log(const char *msg)
{
	int id = manager_id_of_current();

	printk("    [app %d] %s\n", id, msg);
}

void z_impl_svc_heartbeat(void)
{
	manager_app_feed();
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_sensor_read(int32_t *milli_c)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(milli_c, sizeof(*milli_c)));
	return z_impl_svc_sensor_read(milli_c);
}
#include <zephyr/syscalls/svc_sensor_read_mrsh.c>

static inline void z_vrfy_svc_log(const char *msg)
{
	char buf[48];

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
#endif
