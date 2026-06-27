/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A resilient app. It resumes from a saved checkpoint, counts upward saving its
 * progress, and deliberately crashes once. The platform restarts it, and the
 * new instance picks up from the checkpoint instead of starting over - state
 * survived the crash because it was held outside the app's memory domain.
 */

#include "svc.h"

#include <zephyr/kernel.h>

void app_worker(void *arg)
{
	ARG_UNUSED(arg);

	uint32_t count = svc_checkpoint_get();

	if (count == 0U) {
		app_log("worker: fresh start, count = 0");
	} else {
		app_log("worker: resumed from checkpoint after a crash");
	}

	for (;;) {
		count++;
		svc_checkpoint_set(count);

		if (count == 4U) {
			app_log("worker: at count 4, about to crash...");
			/* Deliberate fault: write through a null pointer. */
			volatile uint32_t *bad = (volatile uint32_t *)0;

			*bad = 0xdead;
		}

		if (count >= 8U) {
			app_log("worker: reached count 8, finished cleanly");
			return;
		}

		k_msleep(400);
	}
}
