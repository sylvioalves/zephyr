/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <string.h>

#include "throughput.h"

K_APPMEM_PARTITION_DEFINE(report_part);

static int slot_of(const char *name)
{
	for (int i = 0; i < app_platform_count(); i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0 && st.name != NULL &&
		    strcmp(st.name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static void add_report_part(int id)
{
	struct k_mem_domain *dom = app_platform_domain(id);

	for (int i = 0; i < CONFIG_MAX_DOMAIN_PARTITIONS; i++) {
		if (dom->partitions[i].start == report_part.start &&
		    dom->partitions[i].size == report_part.size) {
			return;
		}
	}
	zassert_equal(k_mem_domain_add_partition(dom, &report_part), 0,
		      "report partition not added");
}

int tp_launch(const char *name, app_entry_t entry, app_caps_t caps)
{
	const struct app_def def = {
		.name = name,
		.source = APP_BUILTIN,
		.entry = entry,
		.caps = caps,
		.stopped = true,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "%s launch failed: %d", name, id);
	add_report_part(id);
	zassert_equal(app_platform_start(id), id, "%s did not start", name);
	return id;
}

bool tp_wait_gone(const char *name, int ms)
{
	for (int i = 0; i < ms / 5; i++) {
		if (slot_of(name) < 0) {
			return true;
		}
		k_msleep(5);
	}
	return slot_of(name) < 0;
}

void tp_sort(uint32_t *v, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		uint32_t x = v[i];
		size_t j = i;

		while (j > 0 && v[j - 1] > x) {
			v[j] = v[j - 1];
			j--;
		}
		v[j] = x;
	}
}

static void *throughput_setup(void)
{
	static bool ready;

	if (!ready) {
		zassert_equal(app_platform_init(), 0, "platform init failed");
		ready = true;
	}
	return NULL;
}

ZTEST_SUITE(net, NULL, throughput_setup, NULL, NULL, NULL);
