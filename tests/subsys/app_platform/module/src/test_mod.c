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
 * The module uses only service syscalls (svc_*) and builds its log line by
 * hand, so it has no libc or supervisor-symbol dependencies to resolve at load
 * time.
 */

#include <zephyr/app_platform/app_services.h>

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

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

	char line[48];
	uint32_t ticks = 0;
	uint32_t acc = 0;

	svc_log("hello: loadable app running");

	for (;;) {
		uint8_t r[4] = {0};

		svc_random(r, sizeof(r));
		acc += (uint32_t)r[0] + r[1] + r[2] + r[3];

		ticks++;
		if ((ticks % 3) == 0) {
			int i = put_str(line, 0, "hello: tick=");

			i = put_u32(line, i, ticks);
			i = put_str(line, i, " acc=");
			i = put_u32(line, i, acc);
			line[i] = '\0';
			svc_log(line);
		}

		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
