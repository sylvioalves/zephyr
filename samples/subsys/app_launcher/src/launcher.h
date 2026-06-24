/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LAUNCHER_LAUNCHER_H_
#define APP_LAUNCHER_LAUNCHER_H_

#include <zephyr/kernel.h>
#include <stdbool.h>

/*
 * Display-agnostic application control API. The launcher UI calls only this;
 * the backend drives the real app-group manager (isolated user-mode apps on a
 * memory-protected target) or a host-friendly stand-in on native_sim. Keeping
 * the UI behind this seam lets the same launcher run in the simulator and on
 * hardware unchanged.
 */

#define LAUNCHER_MAX_APPS 4

enum launcher_app_state {
	LAUNCHER_STOPPED = 0,
	LAUNCHER_RUNNING,
	LAUNCHER_CRASHED,
};

struct launcher_app_info {
	const char             *name;
	enum launcher_app_state state;
	uint32_t                ticks;   /* liveness counter the app advances */
};

/** Initialize the backend (app manager). Call once at startup. */
int launcher_init(void);

/** Number of apps in the catalog. */
int launcher_app_count(void);

/** Snapshot an app's current info by index. Returns 0 on success. */
int launcher_app_get(int idx, struct launcher_app_info *out);

/** Start app @p idx. No-op if already running. */
int launcher_app_start(int idx);

/** Stop/kill app @p idx and reclaim it. No-op if already stopped. */
int launcher_app_kill(int idx);

#endif /* APP_LAUNCHER_LAUNCHER_H_ */
