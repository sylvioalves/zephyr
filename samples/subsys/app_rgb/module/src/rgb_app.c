/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A loadable application that cycles the color of the RGB LED pixel it was
 * granted, logging each step before it is shown and the service result after.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>
#include <stdio.h>

#define RGB_CYCLES  2
#define RGB_STEP_MS 2000
#define RGB_LEVEL   32U

struct rgb_step {
	const char *name;
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

static const struct rgb_step steps[] = {
	{ "red", RGB_LEVEL, 0U, 0U },
	{ "green", 0U, RGB_LEVEL, 0U },
	{ "blue", 0U, 0U, RGB_LEVEL },
	{ "off", 0U, 0U, 0U },
};

void app_main(void *arg)
{
	char line[48];
	int n = 0;
	int failed = 0;

	ARG_UNUSED(arg);

	(void)snprintf(line, sizeof(line), "rgb: %d pixel(s)", app_svc_rgb_count());
	app_svc_log(line);

	for (int c = 0; c < RGB_CYCLES; c++) {
		for (size_t i = 0; i < ARRAY_SIZE(steps); i++) {
			int rc;

			n++;
			(void)snprintf(line, sizeof(line), "rgb: step %d %s", n, steps[i].name);
			app_svc_log(line);
			rc = app_svc_rgb_set(0, steps[i].r, steps[i].g, steps[i].b);
			(void)snprintf(line, sizeof(line), "rgb: app_svc_rgb_set rc=%d", rc);
			app_svc_log(line);
			if (rc != 0) {
				failed++;
			}
			k_msleep(RGB_STEP_MS);
		}
	}

	(void)snprintf(line, sizeof(line), "rgb: %d step(s), %d failed", n, failed);
	app_svc_log(line);
}
EXPORT_SYMBOL(app_main);
