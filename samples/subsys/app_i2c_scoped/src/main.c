/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Scoped I2C capability demo. An app is granted I2C bus 0 at address 0x48 only.
 * Reading 0x48 is allowed (the broker proceeds to the bus); reading any other
 * address is denied at the broker before any bus transaction. The bus is shared
 * platform infrastructure; the app holds a narrow, manifest-defined slice.
 */

#include "i2c_svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

#define GRANTED_ADDR   0x48
#define FORBIDDEN_ADDR 0x50

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	printk("  [app] reading my granted sensor at i2c0 0x%02x...\n",
	       GRANTED_ADDR);
	int rc = app_i2c_read(0, GRANTED_ADDR, 0x00);

	printk("  [app] granted-address read returned %d\n", rc);

	printk("  [app] now trying a DIFFERENT address 0x%02x (a system chip)...\n",
	       FORBIDDEN_ADDR);
	rc = app_i2c_read(0, FORBIDDEN_ADDR, 0x00);
	printk("  [app] forbidden-address read returned %d (%s)\n", rc,
	       rc == -EPERM ? "DENIED, as expected" : "unexpected");
}

int main(void)
{
	printk("\n*** scoped I2C capability: app gets bus0 0x48 only (C6) ***\n");

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	/* The app's manifest grants exactly one (bus, addr): i2c0 @ 0x48. */
	i2c_grant(t, 0, GRANTED_ADDR);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done: app could use only its granted address; others denied ***\n");
	return 0;
}
