/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A loadable app that probes a spread of brokered services, one per syscall
 * shape, and reports which work from inside an llext module. It links no driver
 * code and uses no printf, so it stays small and free of unexported symbols.
 */

#include "svc.h"

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

static void say(const char *label, int rc)
{
	char line[40];
	int i = 0;

	while (*label) {
		line[i++] = *label++;
	}
	line[i++] = ':'; line[i++] = ' ';
	if (rc == 0) {
		line[i++] = 'P'; line[i++] = 'A'; line[i++] = 'S'; line[i++] = 'S';
	} else {
		line[i++] = 'F'; line[i++] = 'A'; line[i++] = 'I'; line[i++] = 'L';
		line[i++] = ' '; line[i++] = 'r'; line[i++] = 'c'; line[i++] = '=';
		int v = rc < 0 ? -rc : rc;

		if (rc < 0) {
			line[i++] = '-';
		}
		if (v >= 100) {
			line[i++] = '0' + (v / 100); v %= 100;
		}
		if (v >= 10) {
			line[i++] = '0' + (v / 10);
		}
		line[i++] = '0' + (v % 10);
	}
	line[i] = '\0';
	svc_log(line);
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("probe: testing service shapes from an llext app");

	/* scalar return */
	say("uptime", svc_uptime() > 0 ? 0 : -1);

	/* sensor scalar */
	say("die_temp", svc_die_temp_milli() != 0 ? 0 : -1);

	/* write buffer to app */
	uint8_t rnd[16] = {0};
	int rc = svc_random(rnd, sizeof(rnd));
	int nonzero = 0;

	for (int j = 0; j < 16; j++) {
		nonzero |= rnd[j];
	}
	say("random", (rc == 0 && nonzero) ? 0 : -1);

	/* string in + scalar in, then string in + scalar out */
	say("kv_set", svc_kv_set("count", 42));
	uint32_t got = 0;

	rc = svc_kv_get("count", &got);
	say("kv_get", (rc == 0 && got == 42) ? 0 : -1);

	/* buffer in + buffer out (crypto) */
	uint8_t digest[32] = {0};

	rc = svc_sha((const uint8_t *)"hello", 5, digest);
	say("sha256", (rc == 0 && digest[0] != 0) ? 0 : -1);

	/* peripheral read into app buffer */
	uint8_t fbuf[16] = {0};

	say("flash_read", svc_flash_read(0, fbuf, sizeof(fbuf)));

	svc_log("probe: done");
	for (;;) {
		svc_heartbeat();
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
