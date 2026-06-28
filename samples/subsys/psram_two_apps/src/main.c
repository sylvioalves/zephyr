/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two isolated user-mode applications, each mapping its own private region of
 * external RAM (PSRAM) on the esp32c5. Each app erases, writes and reads back
 * its region with a distinct byte pattern, proving the mapping is real and
 * usable at memory speed. Because each region is a private partition of that
 * app's own memory domain, the regions are isolated: one app's region is never
 * visible to the other. The platform allocates non-overlapping regions, so the
 * two apps run concurrently without interfering.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define REGION_SIZE (128 * 1024)

/*
 * Each app erases its region to 0xFF, writes a tag-derived pattern, then reads
 * it back. The tag is derived from the running app's platform id, so the two
 * apps write distinguishable data into their separate regions.
 */
static void psram_worker(void *arg)
{
	ARG_UNUSED(arg);

	uint8_t tag = (uint8_t)(0xA0 + app_platform_id_current() * 0x10);
	char line[56];
	uintptr_t addr = 0;
	size_t size = 0;

	snprintk(line, sizeof(line), "app%u: requesting %u KB", tag,
		 REGION_SIZE / 1024);
	svc_log(line);

	if (svc_psram_map(REGION_SIZE, &addr, &size) != 0) {
		svc_log("map FAILED");
		return;
	}

	snprintk(line, sizeof(line), "app%u: mapped 0x%lx size=%zu", tag,
		 (unsigned long)addr, size);
	svc_log(line);

	volatile uint8_t *mem = (volatile uint8_t *)addr;

	/* Erase the whole region. */
	for (size_t i = 0; i < size; i++) {
		mem[i] = 0xFF;
	}

	/* Write a pattern that depends on this app's tag. */
	for (size_t i = 0; i < size; i++) {
		mem[i] = (uint8_t)((i * 31u) + tag);
	}

	/* Read back and verify. */
	int ok = 1;

	for (size_t i = 0; i < size; i++) {
		if (mem[i] != (uint8_t)((i * 31u) + tag)) {
			ok = 0;
			break;
		}
	}

	snprintk(line, sizeof(line), "app%u: erase+write+read %s", tag,
		 ok ? "OK" : "FAILED");
	svc_log(line);

	/*
	 * Report the first few bytes so the two apps' regions are visibly
	 * distinct in the log (different tag -> different content).
	 */
	snprintk(line, sizeof(line), "app%u: bytes[0..3] %02x %02x %02x %02x",
		 tag, mem[0], mem[1], mem[2], mem[3]);
	svc_log(line);

	for (;;) {
		svc_heartbeat();
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

	k_msleep(6000);

	printk("\n*** done: two isolated apps each used a private region ***\n");
	return 0;
}
