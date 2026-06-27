/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Three apps that exercise the platform's liveness model:
 *
 *   busy    - an unmonitored app that runs forever without ever calling
 *             heartbeat. It must never be reclaimed: being busy is not a fault.
 *   crasher - an unmonitored app that dereferences NULL. The platform reclaims
 *             it passively, with no cooperation, the moment its thread dies.
 *   hanger  - a monitored app that opts in to hang detection, feeds the
 *             watchdog for a while, then stops feeding and blocks forever. Only
 *             a monitored app is reclaimed for hanging.
 */

#include "svc.h"

#include <zephyr/kernel.h>

void app_busy(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("busy: running real work, never heartbeats");
	for (;;) {
		volatile uint32_t x = 0;

		for (uint32_t i = 0; i < 200000; i++) {
			x += i;
		}
		svc_log("busy: still working (not reclaimed)");
		k_msleep(700);
	}
}

void app_crasher(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("crasher: about to fault");
	k_msleep(300);

	volatile uint32_t *p = NULL;
	*p = 0xdead;
}

void app_hanger(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("hanger: monitored, feeding watchdog");
	for (int i = 0; i < 3; i++) {
		svc_heartbeat();
		k_msleep(500);
	}

	svc_log("hanger: stopped responding (simulated hang)");
	for (;;) {
		k_msleep(K_TICKS_FOREVER);
	}
}
