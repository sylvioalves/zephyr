/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "i2c_svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/internal/syscall_handler.h>

/* The platform owns the bus controller (from devicetree). */
static const struct device *const bus0 = DEVICE_DT_GET(DT_NODELABEL(i2c0));

/* Per-app manifest grants: which (bus, addr) each app thread may touch. */
struct i2c_grant_entry {
	k_tid_t  tid;
	uint8_t  bus;
	uint16_t addr;
};

#define MAX_GRANTS 8
static struct i2c_grant_entry grants[MAX_GRANTS];

void i2c_grant(k_tid_t tid, uint8_t bus, uint16_t addr)
{
	for (int i = 0; i < MAX_GRANTS; i++) {
		if (grants[i].tid == NULL) {
			grants[i].tid = tid;
			grants[i].bus = bus;
			grants[i].addr = addr;
			return;
		}
	}
}

static bool granted(k_tid_t tid, uint8_t bus, uint16_t addr)
{
	for (int i = 0; i < MAX_GRANTS; i++) {
		if (grants[i].tid == tid && grants[i].bus == bus &&
		    grants[i].addr == addr) {
			return true;
		}
	}
	return false;
}

int z_impl_app_i2c_read(uint8_t bus, uint16_t addr, uint8_t reg)
{
	/* Enforce the manifest scope BEFORE any bus access. */
	if (!granted(k_current_get(), bus, addr)) {
		printk("    [i2c-broker] DENIED app i2c%u @ 0x%02x (not in manifest)\n",
		       bus, addr);
		return -EPERM;
	}

	if (bus != 0 || !device_is_ready(bus0)) {
		return -ENODEV;
	}

	uint8_t val = 0;
	int rc = i2c_reg_read_byte(bus0, addr, reg, &val);

	printk("    [i2c-broker] GRANTED app i2c%u @ 0x%02x reg 0x%02x -> rc=%d\n",
	       bus, addr, reg, rc);
	return (rc == 0) ? val : rc;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_i2c_read(uint8_t bus, uint16_t addr, uint8_t reg)
{
	return z_impl_app_i2c_read(bus, addr, reg);
}
#include <zephyr/syscalls/app_i2c_read_mrsh.c>
#endif
