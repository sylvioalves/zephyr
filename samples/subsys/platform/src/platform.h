/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLATFORM_PLATFORM_H_
#define PLATFORM_PLATFORM_H_

#include <zephyr/kernel.h>

/*
 * The integrated platform: one image where the installed-app registry, the app
 * manager, the services, and supervision run together. Boot loads the installed
 * apps from flash, launches each as an isolated group, and supervises them so a
 * misbehaving app is reclaimed without taking the system or other apps down.
 *
 * Apps are entries in a catalog. Today each entry is a built-in function; the
 * entry shape (name + entry pointer + source) is the same one a loadable .llext
 * app would fill, so the app-store path slots in without changing the manager.
 */

#define PLATFORM_MAX_APPS 4

enum app_source {
	APP_BUILTIN = 0,   /* compiled into the firmware            */
	APP_LOADABLE,      /* loaded from flash as an llext (future) */
};

typedef void (*app_entry_t)(void *arg);

/* A catalog entry: how the platform knows how to run an app by name. */
struct app_def {
	const char     *name;
	enum app_source source;
	app_entry_t     entry;   /* for APP_BUILTIN */
};

/* Look up a catalog entry by name (the manager resolves installed names here). */
const struct app_def *platform_catalog_find(const char *name);

/* ---- services the apps use (the SDK surface, kept tiny for the demo) ---- */
int  plat_sensor_read(int32_t *milli_c);
void plat_log(const char *app, const char *msg);
void plat_heartbeat(void);   /* an app proves liveness */

#endif /* PLATFORM_PLATFORM_H_ */
