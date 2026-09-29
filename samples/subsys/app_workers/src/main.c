/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define ITEMS 5

K_APPMEM_PARTITION_DEFINE(work_part);

struct work_area {
	int empty;
	int full;
	int item;
	int sum;
};

K_APP_BMEM(work_part) static struct work_area work;

static void producer(void *arg)
{
	ARG_UNUSED(arg);
	for (int i = 1; i <= ITEMS; i++) {
		(void)app_svc_sem_take(work.empty, -1);
		work.item = i * i;
		(void)app_svc_sem_give(work.full);
	}
}

static void consumer_app(void *arg)
{
	char line[40];
	int worker;

	ARG_UNUSED(arg);
	work.empty = app_svc_sem_init(1, 1);
	work.full = app_svc_sem_init(0, 1);
	worker = app_svc_thread_spawn(producer, NULL, 0);
	if (work.empty < 0 || work.full < 0 || worker < 0) {
		app_svc_log("workers: setup failed");
		return;
	}

	for (int i = 0; i < ITEMS; i++) {
		(void)app_svc_sem_take(work.full, -1);
		work.sum += work.item;
		snprintk(line, sizeof(line), "consumer got %d", work.item);
		app_svc_log(line);
		(void)app_svc_sem_give(work.empty);
	}
	(void)app_svc_thread_join(worker, -1);
}

int main(void)
{
	const struct app_def def = {
		.name = "workers",
		.source = APP_BUILTIN,
		.entry = consumer_app,
		.caps = APP_CAP_LOG | APP_CAP_THREAD,
	};
	struct app_platform_status st;
	int id;

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	id = app_platform_launch(&def);
	if (id < 0) {
		printk("launch failed: %d\n", id);
		return 0;
	}
#ifdef CONFIG_USERSPACE
	k_mem_domain_add_partition(app_platform_domain(id), &work_part);
#endif

	do {
		k_msleep(10);
	} while (app_platform_status(id, &st) == 0 && st.active);

	printk("done: 2 threads of one app passed %d items, sum %d\n", ITEMS, work.sum);
	return 0;
}
