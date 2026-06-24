/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Sensor services: the platform announces a catalog of typed sensor services.
 * Apps discover what is available and read values by type, never touching the
 * peripheral, bus, or driver. Read-only sensors are shared: any number of apps
 * read them in parallel, like the network service. Here two isolated user-mode
 * apps both read the temperature concurrently.
 */

#include "sensor_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern void sensor_catalog_init(void);

#define STACK_SIZE 2048
#define APP_PRIO   5
#define TEMP_IDX   0   /* the temperature service is catalog entry 0 */

K_THREAD_STACK_DEFINE(stack_a, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_b, STACK_SIZE);
static struct k_thread th_a, th_b;

/* A reader app: continuously reads the temperature service in user mode. */
static void reader_app(void *name_p, void *p2, void *p3)
{
	ARG_UNUSED(p2); ARG_UNUSED(p3);
	const char *name = name_p;

	for (int i = 0; ; i++) {
		int32_t milli;
		int rc = svc_sensor_read(TEMP_IDX, &milli);

		if (rc == 0) {
			printk("  [%s] #%d temperature = %d.%03d C\n",
			       name, i, milli / 1000, (milli < 0 ? -milli : milli) % 1000);
		} else {
			printk("  [%s] #%d read rc=%d\n", name, i, rc);
		}
		k_msleep(700);
	}
}

static void announce_catalog(void)
{
	int n = svc_sensor_count();

	printk("\nplatform announces %d sensor services:\n", n);
	for (int i = 0; i < n; i++) {
		char name[24], unit[12];
		bool present;

		if (svc_sensor_info(i, name, sizeof(name), unit, sizeof(unit),
				    &present) == 0) {
			printk("  [%2d] %-16s (%-8s) %s\n", i, name, unit,
			       present ? "PRESENT" : "announced");
		}
	}
}

int main(void)
{
	printk("\n*** sensor services: announced catalog, parallel reads (C6) ***\n");

	sensor_catalog_init();
	announce_catalog();

	printk("\ntwo isolated apps reading the temperature service in parallel:\n");

	k_tid_t ta = k_thread_create(&th_a, stack_a, STACK_SIZE, reader_app,
				     "app-A", NULL, NULL, APP_PRIO, K_USER,
				     K_FOREVER);
	k_tid_t tb = k_thread_create(&th_b, stack_b, STACK_SIZE, reader_app,
				     "app-B", NULL, NULL, APP_PRIO, K_USER,
				     K_FOREVER);

	k_thread_start(ta);
	k_thread_start(tb);
	return 0;
}
