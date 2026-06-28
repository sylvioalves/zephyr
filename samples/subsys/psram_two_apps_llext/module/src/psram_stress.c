/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Loadable application module. It maps a private region of external RAM and
 * runs a sustained read/write/verify stress over it for ten seconds. Two
 * instances of this same module run concurrently as separate isolated apps,
 * each with its own private region, so any corruption or cross-app
 * interference surfaces as a verify mismatch. A per-instance tag, derived from
 * the mapped base address, distinguishes the two instances' data.
 *
 * The module only uses the user-mode service syscalls (svc_*) and builds its
 * log strings by hand, so it has no dependency on libc symbols that would need
 * to be resolved at load time.
 */

#include <zephyr/app_platform/app_services.h>

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define REGION_SIZE (128 * 1024)
#define STRESS_MS   10000

static int put_str(char *out, int i, const char *s)
{
	while (*s) {
		out[i++] = *s++;
	}
	return i;
}

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

	char line[64];
	int i;
	uintptr_t addr = 0;
	size_t size = 0;

	svc_log("app: requesting external RAM region");

	if (svc_psram_map(REGION_SIZE, &addr, &size) != 0) {
		svc_log("map FAILED");
		for (;;) {
			svc_heartbeat();
			k_msleep(1000);
		}
	}

	/*
	 * Derive a per-instance tag from the mapped base address. The platform
	 * hands each app a distinct, non-overlapping region, so this gives the
	 * two instances different patterns without needing any shared state.
	 */
	uint8_t tag = (uint8_t)(0xA0 + ((addr >> 18) & 0x0F) * 0x10);

	i = put_str(line, 0, "app");
	i = put_u32(line, i, tag);
	i = put_str(line, i, ": mapped, size=");
	i = put_u32(line, i, (uint32_t)size);
	line[i] = '\0';
	svc_log(line);

	volatile uint8_t *mem = (volatile uint8_t *)addr;

	uint32_t iters = 0;
	uint32_t errors = 0;
	int64_t deadline = k_uptime_get() + STRESS_MS;

	/*
	 * The hot loop intentionally makes no service syscalls. These apps are
	 * not monitored, so a heartbeat would be a no-op, and issuing a syscall
	 * every pass while a second loaded app is doing the same forces a heavy
	 * stream of user/kernel transitions that the platform does not need here.
	 * The work itself - direct loads and stores to the mapped region - needs
	 * no kernel help.
	 */
	while (k_uptime_get() < deadline) {
		uint8_t seed = (uint8_t)(tag + iters);

		for (size_t j = 0; j < size; j++) {
			mem[j] = (uint8_t)((j * 31u) + seed);
		}
		for (size_t j = 0; j < size; j++) {
			if (mem[j] != (uint8_t)((j * 31u) + seed)) {
				errors++;
				break;
			}
		}

		iters++;
	}

	i = put_str(line, 0, "app");
	i = put_u32(line, i, tag);
	i = put_str(line, i, ": 10s stress iters=");
	i = put_u32(line, i, iters);
	i = put_str(line, i, " err=");
	i = put_u32(line, i, errors);
	i = put_str(line, i, errors ? " FAILED" : " OK");
	line[i] = '\0';
	svc_log(line);

	for (;;) {
		svc_heartbeat();
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
