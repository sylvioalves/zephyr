/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Messaging benchmark. Loads one isolated app that times the messaging path and
 * reports raw cycle counts. The cycle rate is printed so the figures convert to
 * latency and throughput.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t bench_app[] __aligned(8) = {
#include "bench.inc"
};
static const uint8_t bench_hash[32] = {
#include "bench.sha256.inc"
};

int main(void)
{
	printk("\n*** messaging benchmark (C6) ***\n");
	printk("cycle_rate_hz=%u\n", (unsigned int)sys_clock_hw_cycles_per_sec());

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("bench", bench_app, ARRAY_SIZE(bench_app),
					bench_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("install failed: %d\n", rc);
		return 0;
	}

	app_platform_launch_llext("bench", ext, (app_entry_t)entry, NULL,
				  APP_CAP_LOG | APP_CAP_IPC | APP_CAP_TIME);

	k_msleep(8000);

	printk("\n*** benchmark done ***\n");
	return 0;
}
