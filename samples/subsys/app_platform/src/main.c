/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The consolidated platform: one SDK, one service registry, three capability
 * tiers, and the multi-threaded app model - all behind a single header. A
 * multi-threaded app discovers what the platform offers, then exercises an open
 * service (sensor), a scoped service (i2c, granted one address), and an admin
 * service (wifi config, denied because the app is not admin).
 */

#include "app_sdk.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <stdio.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

extern void platform_app_register(k_tid_t tid, bool admin);
extern void platform_grant_i2c(k_tid_t tid, uint8_t bus, uint16_t addr);
extern void app_rt_set_domain(struct k_mem_domain *dom);

K_APPMEM_PARTITION_DEFINE(app_part);
static struct k_mem_domain app_domain;

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static const char *tier_name(int t)
{
	return (t == 0) ? "open" : (t == 1) ? "scoped" : "admin";
}

/* A second thread of the same app - shares the app's domain and capabilities. */
static void app_worker(void *arg)
{
	ARG_UNUSED(arg);
	app_log("worker thread: also part of this app, shares its capabilities");

	int32_t t;

	if (app_sensor_read("temperature", &t) == 0) {
		app_log("worker read the sensor too (shared open capability)");
	}
}

static void app_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* 1. Discover what the platform offers (the registry). */
	int n = app_service_count();

	app_log("discovering platform services:");
	for (int i = 0; i < n; i++) {
		char name[20];
		int tier;

		if (app_service_info(i, name, sizeof(name), &tier) == 0) {
			char line[48];

			snprintf(line, sizeof(line), "service '%s' (%s)", name,
				 tier_name(tier));
			app_log(line);
		}
	}

	/* 2. Multi-threaded: spawn another thread in this same app. */
	int worker = app_thread_spawn(app_worker, NULL);

	/* 3. Open capability: read a sensor (any app may). */
	int32_t milli;

	if (app_sensor_read("temperature", &milli) == 0) {
		char line[48];

		snprintf(line, sizeof(line), "[open] temperature read ok (%d milli-C)",
			 milli);
		app_log(line);
	}

	/* 4. Scoped capability: granted i2c0 @ 0x48, denied elsewhere. */
	app_i2c_read(0, 0x48, 0x00);   /* granted */
	app_i2c_read(0, 0x50, 0x00);   /* denied  */

	/* 5. Admin capability: this app is not admin -> denied. */
	app_admin_wifi_set("HomeNet", "secret123");

	/* Reap the worker so it is joined, not self-aborted from user mode. */
	if (worker >= 0) {
		app_thread_join(worker);
	}

	app_log("app done");
}

int main(void)
{
	printk("\n*** unified app platform: one SDK, registry, tiers, threads (C6) ***\n");

	k_mem_domain_init(&app_domain, 0, NULL);
	struct k_mem_partition *p = &app_part;

	k_mem_domain_add_partition(&app_domain, p);
	app_rt_set_domain(&app_domain);

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_main,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);
	k_mem_domain_add_thread(&app_domain, t);

	/* Manifest: this app is a normal (non-admin) app, granted i2c0 @ 0x48. */
	platform_app_register(t, false);
	platform_grant_i2c(t, 0, 0x48);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done: one header, registry-driven, tier-enforced platform ***\n");
	return 0;
}
