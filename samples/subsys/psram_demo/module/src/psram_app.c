/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A loadable app that maps a region of external RAM into its own domain and
 * uses it directly. It writes a pattern across the whole region and reads it
 * back to prove the mapping is real and writable, then times a large traversal
 * to show the access is at memory speed, not through a per-access syscall.
 */

#include <zephyr/app_platform/app_services.h>

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

static int put_u32(char *out, int i, uint32_t v)
{
	char tmp[10];
	int n = 0;

	if (v == 0) {
		out[i++] = '0';
		return i;
	}
	while (v > 0) {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	}
	while (n > 0) {
		out[i++] = tmp[--n];
	}
	return i;
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	uintptr_t addr = 0;
	size_t size = 0;

	svc_log("psram: requesting a 256 KB region");
	int rc = svc_psram_map(256 * 1024, &addr, &size);

	if (rc != 0) {
		svc_log("psram: map FAILED");
		for (;;) {
			svc_heartbeat();
			k_msleep(1000);
		}
	}

	{
		char line[48];
		int i = 0;
		const char *p = "psram: mapped at 0x";

		while (*p) {
			line[i++] = *p++;
		}
		for (int s = 28; s >= 0; s -= 4) {
			uint8_t nib = (addr >> s) & 0xF;

			line[i++] = nib < 10 ? '0' + nib : 'a' + (nib - 10);
		}
		const char *q = " size=";

		while (*q) {
			line[i++] = *q++;
		}
		i = put_u32(line, i, (uint32_t)size);
		line[i] = '\0';
		svc_log(line);
	}

	/* Direct write across the whole region - no syscall per access. */
	volatile uint8_t *mem = (volatile uint8_t *)addr;

	for (size_t i = 0; i < size; i++) {
		mem[i] = (uint8_t)(i * 7 + 1);
	}

	/* Read back and verify. */
	int ok = 1;

	for (size_t i = 0; i < size; i++) {
		if (mem[i] != (uint8_t)(i * 7 + 1)) {
			ok = 0;
			break;
		}
	}
	svc_log(ok ? "psram: write+read of full region OK"
		   : "psram: verify FAILED");

	/* Time a large traversal to show it is memory-speed access. */
	uint32_t t0 = svc_cycles();
	uint64_t sum = 0;

	for (int pass = 0; pass < 10; pass++) {
		for (size_t i = 0; i < size; i++) {
			sum += mem[i];
		}
	}
	uint32_t cyc = svc_cycles() - t0;

	{
		char line[48];
		int i = 0;
		const char *p = "psram: 10x256KB read cyc=";

		while (*p) {
			line[i++] = *p++;
		}
		i = put_u32(line, i, cyc);
		line[i++] = ' '; line[i++] = 's'; line[i++] = '=';
		i = put_u32(line, i, (uint32_t)(sum & 0xffff));
		line[i] = '\0';
		svc_log(line);
	}

	svc_log("psram: done");
	for (;;) {
		svc_heartbeat();
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
