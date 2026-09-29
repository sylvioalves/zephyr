/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_heap.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define NODES     10
#define HEAP_SIZE 1024U

K_APPMEM_PARTITION_DEFINE(result_part);

struct result {
	int sum;
	bool reusable;
};

K_APP_BMEM(result_part) static struct result res;

struct node {
	struct node *next;
	int value;
};

static void list_app(void *arg)
{
	struct node *head = NULL;
	char line[48];
	void *big;

	ARG_UNUSED(arg);
	for (int i = 1; i <= NODES; i++) {
		struct node *n = app_malloc(sizeof(*n));

		if (n == NULL) {
			app_svc_log("heap: out of memory");
			return;
		}
		n->value = i * i;
		n->next = head;
		head = n;
	}

	for (struct node *n = head; n != NULL; n = n->next) {
		res.sum += n->value;
	}
	snprintk(line, sizeof(line), "list of %d nodes, sum %d", NODES, res.sum);
	app_svc_log(line);

	while (head != NULL) {
		struct node *next = head->next;

		app_free(head);
		head = next;
	}

	big = app_malloc(HEAP_SIZE / 2U);
	res.reusable = (big != NULL);
	app_free(big);
}

int main(void)
{
	const struct app_def def = {
		.name = "heap",
		.source = APP_BUILTIN,
		.entry = list_app,
		.caps = APP_CAP_LOG,
		.heap_size = HEAP_SIZE,
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
	k_mem_domain_add_partition(app_platform_domain(id), &result_part);
#endif
	if (app_platform_status(id, &st) == 0) {
		printk("heap region of %zu bytes for %u requested\n", st.heap_bytes, HEAP_SIZE);
	}

	do {
		k_msleep(10);
	} while (app_platform_status(id, &st) == 0 && st.active);

	printk("done: a list of %d nodes on the app heap, sum %d, heap %s\n", NODES, res.sum,
	       res.reusable ? "reusable" : "not reusable");
	return 0;
}
