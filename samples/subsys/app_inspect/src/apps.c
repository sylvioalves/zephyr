/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Worker apps with different resource profiles, plus an inspector app that holds
 * the inspect capability and prints a top-style table of every group's stack and
 * CPU use. The inspector reads the others only through the brokered svc_app_info
 * call; an app without APP_CAP_INSPECT is refused.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/kernel.h>
#include <stdio.h>

/* spinner: burns CPU continuously - should show a high cycle share. */
void app_spinner(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("spinner: started");
	for (;;) {
		volatile uint32_t x = 0;

		for (uint32_t i = 0; i < 1000000; i++) {
			x += i;
		}
	}
}

/* sleeper: mostly idle - should show a low cycle share. */
void app_sleeper(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("sleeper: started");
	for (;;) {
		k_msleep(1000);
	}
}

/* deep: recurses to grow its stack high-water mark. */
static void recurse(int depth)
{
	volatile char pad[64];

	pad[0] = (char)depth;
	if (depth > 0) {
		recurse(depth - 1);
	}
	(void)pad[0];
}

void app_deep(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("deep: started");
	recurse(6);
	for (;;) {
		k_msleep(1000);
	}
}

/* inspector: prints a status table for all groups, every second. */
void app_inspector(void *arg)
{
	ARG_UNUSED(arg);

	int n = svc_app_count();

	for (int round = 0; round < 4; round++) {
		k_msleep(1000);
		svc_log("---- app status ----");
		for (int id = 0; id < n; id++) {
			struct svc_app_info info;

			if (svc_app_info(id, &info) != 0) {
				continue;
			}

			uint32_t share = 0;

			if (info.cpu_total != 0) {
				share = (uint32_t)((info.cpu_cycles * 1000U) / info.cpu_total);
			}

			char line[80];

			snprintf(line, sizeof(line),
				 "%-9s stack %u/%u B  cpu %u.%u%%%s",
				 info.name, info.stack_used, info.stack_size,
				 share / 10U, share % 10U,
				 info.monitored ? "  [mon]" : "");
			svc_log(line);
		}
	}
	svc_log("inspector: done");
}
