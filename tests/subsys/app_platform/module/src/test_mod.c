/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Minimal loadable application: it announces itself once, then loops forever,
 * doing a little random work and sleeping one second between iterations. Many
 * instances of this module run in parallel as isolated user-mode apps so the
 * maximum number of concurrent loadable apps can be measured.
 *
 * The module calls only the functions of app_sdk.h and builds its log line by
 * hand, so it resolves nothing but the platform's exported functions at load
 * time.
 */

#include <zephyr/app_platform/app_sdk.h>

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

/* Argument that makes the module exercise its heap and return. */
#define HEAP_PROBE ((void *)0x4850U)

/* Argument that makes the module check the kernel services and return. */
#define TIME_PROBE ((void *)0x5449U)

/* Argument that makes the module check 64-bit division and return. */
#define DIV_PROBE ((void *)0x4449U)

static bool div_probe(uintptr_t divisor)
{
	uint64_t n = (uint64_t)app_uptime_get() + 0x123456789aULL;
	int64_t s = -(int64_t)n;
	uint64_t q = n / divisor;
	uint64_t r = n % divisor;
	int64_t sq = s / (int64_t)divisor;
	int64_t sr = s % (int64_t)divisor;

	return q * divisor + r == n && r < divisor && sq * (int64_t)divisor + sr == s && sr <= 0;
}

static bool time_probe(void)
{
	int64_t start = app_uptime_get();

	app_yield();
	if (app_sleep_us(2000) != 0 || app_sleep_ms(20) != 0) {
		return false;
	}
	return app_uptime_get() - start >= 20;
}

static bool heap_probe(void)
{
	uint8_t *p = app_malloc(64);
	uint8_t *q;

	if (p == NULL) {
		return false;
	}
	for (int i = 0; i < 64; i++) {
		p[i] = (uint8_t)i;
	}
	q = app_realloc(p, 128);
	if (q == NULL) {
		app_free(p);
		return false;
	}
	for (int i = 0; i < 64; i++) {
		if (q[i] != (uint8_t)i) {
			return false;
		}
	}
	app_free(q);
	p = app_calloc(4, 8);
	if (p == NULL || p[0] != 0U) {
		return false;
	}
	app_free(p);
	return true;
}

void app_main(void *arg)
{
	if (arg == HEAP_PROBE) {
		while (!heap_probe()) {
			app_sleep_ms(1000);
		}
		return;
	}
	if (arg == DIV_PROBE) {
		while (!div_probe((uintptr_t)arg)) {
			app_sleep_ms(1000);
		}
		return;
	}
	if (arg == TIME_PROBE) {
		while (!time_probe()) {
			app_sleep_ms(1000);
		}
		return;
	}

	char line[48];
	uint32_t ticks = 0;
	uint32_t acc = 0;

	app_svc_log("hello: loadable app running");

	for (;;) {
		uint8_t r[4] = {0};

		app_svc_random(r, sizeof(r));
		acc += (uint32_t)r[0] + r[1] + r[2] + r[3];

		ticks++;
		if ((ticks % 3) == 0) {
			int i = put_str(line, 0, "hello: tick=");

			i = put_u32(line, i, ticks);
			i = put_str(line, i, " acc=");
			i = put_u32(line, i, acc);
			line[i] = '\0';
			app_svc_log(line);
		}

		app_sleep_ms(1000);
	}
}
EXPORT_SYMBOL(app_main);
