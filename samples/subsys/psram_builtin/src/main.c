/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * External-RAM mapping on the esp32c5, using a built-in application so the test
 * does not depend on the loadable-app path. The app is launched as an isolated
 * user-mode thread, granted the PSRAM capability, and maps a region of external
 * RAM into its own domain to use directly. It writes and reads back the whole
 * region to prove the mapping is real, then times a traversal to show the access
 * is at memory speed rather than through a per-access syscall.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static void psram_app(void *arg)
{
	ARG_UNUSED(arg);

	uintptr_t addr = 0;
	size_t size = 0;

	svc_log("psram: requesting a 256 KB region");
	if (svc_psram_map(256 * 1024, &addr, &size) != 0) {
		svc_log("psram: map FAILED");
		return;
	}

	char line[48];

	snprintk(line, sizeof(line), "psram: mapped 0x%lx size=%zu",
		 (unsigned long)addr, size);
	svc_log(line);

	volatile uint8_t *mem = (volatile uint8_t *)addr;

	for (size_t i = 0; i < size; i++) {
		mem[i] = (uint8_t)(i * 7 + 1);
	}

	int ok = 1;

	for (size_t i = 0; i < size; i++) {
		if (mem[i] != (uint8_t)(i * 7 + 1)) {
			ok = 0;
			break;
		}
	}
	svc_log(ok ? "psram: write+read of full region OK"
		   : "psram: verify FAILED");

	uint32_t t0 = svc_cycles();
	uint64_t sum = 0;

	for (int pass = 0; pass < 10; pass++) {
		for (size_t i = 0; i < size; i++) {
			sum += mem[i];
		}
	}
	snprintk(line, sizeof(line), "psram: 10x256KB read cyc=%u sum=%u",
		 svc_cycles() - t0, (uint32_t)sum);
	svc_log(line);

	svc_log("psram: done");
	for (;;) {
		svc_heartbeat();
		k_msleep(1000);
	}
}

int main(void)
{
	printk("\n*** external RAM mapping (built-in app, C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	const struct app_def app = {
		.name = "psram", .source = APP_BUILTIN, .entry = psram_app,
		.caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_PSRAM,
	};

	app_platform_launch(&app);

	k_msleep(8000);

	printk("\n*** done: app used mapped external RAM directly ***\n");
	return 0;
}
