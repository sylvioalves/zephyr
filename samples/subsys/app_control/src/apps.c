/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The manager app and the workers it controls. The manager is just another
 * isolated app, but it holds the control capability, so it can start and stop
 * workers at runtime through brokered service calls - a launcher built as an
 * app, not baked into the kernel.
 */

#include "svc.h"
#include "app_caps.h"

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/kernel.h>
#include <string.h>

/* worker: a plain app that ticks until it is stopped. */
static void app_worker(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("worker: started");
	for (;;) {
		svc_log("worker: tick");
		svc_heartbeat();
		k_msleep(500);
	}
}

/* The workers the manager may start, resolved by name by the broker. */
static const struct app_def workers[] = {
	{ "worker-a", APP_BUILTIN, app_worker, 0 },
	{ "worker-b", APP_BUILTIN, app_worker, 0 },
};

const struct app_def *control_catalog_find(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(workers); i++) {
		if (strcmp(workers[i].name, name) == 0) {
			return &workers[i];
		}
	}
	return NULL;
}

/* Sleep while proving liveness, so the manager is not reclaimed as hung. */
static void wait_alive(int ms)
{
	for (int t = 0; t < ms; t += 200) {
		svc_heartbeat();
		k_msleep(200);
	}
}

/* manager: starts two workers, lets them run, stops one, then the other. */
void app_manager(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("manager: starting worker-a and worker-b");
	int a = svc_ctrl_start("worker-a");
	int b = svc_ctrl_start("worker-b");

	wait_alive(1500);

	svc_log("manager: stopping worker-a");
	svc_ctrl_stop(a);

	wait_alive(1500);

	svc_log("manager: stopping worker-b");
	svc_ctrl_stop(b);

	svc_log("manager: done - managed the workers at runtime");
	for (;;) {
		svc_heartbeat();
		k_msleep(500);
	}
}
