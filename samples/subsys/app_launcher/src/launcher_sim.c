/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Launcher backend for native_sim. The simulator does not provide the
 * memory-protected userspace the hardware backend uses, so here each app is a
 * plain supervised thread. This exercises the launcher UI and the full
 * lifecycle (start / run / kill / crash-and-reclaim) so the UI can be built and
 * iterated on the host; the hardware backend swaps this for the real app-group
 * manager behind the same launcher.h API.
 */

#include "launcher.h"

#include <zephyr/kernel.h>

#define APP_STACK_SIZE 1024
#define APP_PRIO       7

struct sim_app {
	const char             *name;
	bool                    crashy;   /* fault a few ticks after start */
	enum launcher_app_state state;
	uint32_t                ticks;
	struct k_thread         thread;
	bool                    stop;
};

K_THREAD_STACK_ARRAY_DEFINE(app_stacks, LAUNCHER_MAX_APPS, APP_STACK_SIZE);

static struct sim_app apps[] = {
	{ .name = "sensors",  .crashy = false },
	{ .name = "telemetry", .crashy = false },
	{ .name = "buggy",    .crashy = true  },
};

#define APP_COUNT ((int)ARRAY_SIZE(apps))

static void app_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2); ARG_UNUSED(p3);
	struct sim_app *a = p1;

	a->ticks = 0;
	while (!a->stop) {
		a->ticks++;
		if (a->crashy && a->ticks >= 6) {
			/* Simulate a fault: the supervisor marks it crashed and
			 * reclaims it, mirroring the hardware containment path.
			 */
			a->state = LAUNCHER_CRASHED;
			return;
		}
		k_msleep(500);
	}
	a->state = LAUNCHER_STOPPED;
}

int launcher_init(void)
{
	for (int i = 0; i < APP_COUNT; i++) {
		apps[i].state = LAUNCHER_STOPPED;
		apps[i].ticks = 0;
	}
	return 0;
}

int launcher_app_count(void)
{
	return APP_COUNT;
}

int launcher_app_get(int idx, struct launcher_app_info *out)
{
	if (idx < 0 || idx >= APP_COUNT) {
		return -EINVAL;
	}
	out->name = apps[idx].name;
	out->state = apps[idx].state;
	out->ticks = apps[idx].ticks;
	return 0;
}

int launcher_app_start(int idx)
{
	if (idx < 0 || idx >= APP_COUNT) {
		return -EINVAL;
	}
	struct sim_app *a = &apps[idx];

	if (a->state == LAUNCHER_RUNNING) {
		return 0;
	}

	a->stop = false;
	a->ticks = 0;
	a->state = LAUNCHER_RUNNING;
	k_thread_create(&a->thread, app_stacks[idx], APP_STACK_SIZE,
			app_thread, a, NULL, NULL, APP_PRIO, 0, K_NO_WAIT);
	return 0;
}

int launcher_app_kill(int idx)
{
	if (idx < 0 || idx >= APP_COUNT) {
		return -EINVAL;
	}
	struct sim_app *a = &apps[idx];

	if (a->state == LAUNCHER_STOPPED) {
		return 0;
	}

	a->stop = true;
	k_thread_abort(&a->thread);
	a->state = LAUNCHER_STOPPED;
	a->ticks = 0;
	return 0;
}
