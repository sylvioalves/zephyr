/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two isolated user-mode applications, each mapping its own private region of
 * external RAM (PSRAM) on the esp32c5. The two apps run a sustained, concurrent
 * read/write/verify stress over their separate regions for ten seconds. Each
 * region is a private partition of that app's own memory domain, so the apps
 * are isolated: one app's region is never visible to the other. The platform
 * allocates non-overlapping regions, so both can hammer external RAM at the
 * same time. Any corruption - whether a real PSRAM fault or cross-app
 * interference - shows up as a verify mismatch.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define REGION_SIZE   (128 * 1024)
#define STRESS_MS     10000

/*
 * Each app maps its private region, then for STRESS_MS milliseconds repeatedly
 * writes the whole region with a pattern that rotates every iteration and reads
 * it all back, verifying every byte. The pattern depends on the app's tag and
 * the iteration counter so a stale or cross-app value is caught immediately.
 * The tag is derived from the running app's platform id.
 */
static void psram_worker(void *arg)
{
	ARG_UNUSED(arg);

	uint8_t tag = (uint8_t)(0xA0 + app_platform_id_current() * 0x10);
	char line[64];
	uintptr_t addr = 0;
	size_t size = 0;

	snprintk(line, sizeof(line), "app%u: requesting %u KB", tag,
		 REGION_SIZE / 1024);
	app_svc_log(line);

	if (app_svc_psram_map(REGION_SIZE, &addr, &size) != 0) {
		app_svc_log("map FAILED");
		return;
	}

	snprintk(line, sizeof(line), "app%u: mapped 0x%lx size=%zu", tag,
		 (unsigned long)addr, size);
	app_svc_log(line);

	volatile uint8_t *mem = (volatile uint8_t *)addr;

	uint32_t iters = 0;
	uint32_t errors = 0;
	uint64_t bytes = 0;
	int64_t deadline = k_uptime_get() + STRESS_MS;

	while (k_uptime_get() < deadline) {
		uint8_t seed = (uint8_t)(tag + iters);

		/* Write the whole region with this iteration's pattern. */
		for (size_t i = 0; i < size; i++) {
			mem[i] = (uint8_t)((i * 31u) + seed);
		}

		/* Read it all back and verify every byte. */
		for (size_t i = 0; i < size; i++) {
			if (mem[i] != (uint8_t)((i * 31u) + seed)) {
				errors++;
				break;
			}
		}

		bytes += (uint64_t)size * 2u;
		iters++;
	}

	snprintk(line, sizeof(line),
		 "app%u: 10s stress iters=%u err=%u MB=%u %s", tag, iters,
		 errors, (uint32_t)(bytes / (1024 * 1024)),
		 errors ? "FAILED" : "OK");
	app_svc_log(line);

	for (;;) {
		app_svc_heartbeat();
		k_msleep(1000);
	}
}

int main(void)
{
	printk("\n*** two-app private external RAM (C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	const struct app_def app_a = {
		.name = "psram-a",
		.source = APP_BUILTIN,
		.entry = psram_worker,
		.caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_PSRAM,
	};
	const struct app_def app_b = {
		.name = "psram-b",
		.source = APP_BUILTIN,
		.entry = psram_worker,
		.caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_PSRAM,
	};

	app_platform_launch(&app_a);
	app_platform_launch(&app_b);

	/* Let both apps run the full 10 s stress plus reporting time. */
	k_msleep(STRESS_MS + 3000);

	printk("\n*** done: two isolated apps each used a private region ***\n");
	return 0;
}
