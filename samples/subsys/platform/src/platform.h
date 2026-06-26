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

#define PLATFORM_MAX_APPS 5

enum app_source {
	APP_BUILTIN = 0,   /* compiled into the firmware            */
	APP_LOADABLE,      /* loaded from flash as an llext (future) */
};

typedef void (*app_entry_t)(void *arg);

/*
 * Per-app capabilities. Each catalog entry declares the services it is granted;
 * the broker rejects any service call whose capability bit is clear, so an app
 * reaches only the subsystems it was installed with.
 */
#define CAP_SENSOR  BIT(0)   /* read the die temperature        */
#define CAP_GPIO    BIT(1)   /* drive the status LED            */
#define CAP_PWM     BIT(2)   /* set the PWM duty                */
#define CAP_FLASH   BIT(3)   /* read/write its flash slot       */
#define CAP_RTC     BIT(4)   /* read the wall clock             */

/* A catalog entry: how the platform knows how to run an app by name. */
struct app_def {
	const char     *name;
	enum app_source source;
	app_entry_t     entry;   /* for APP_BUILTIN */
	uint32_t        caps;    /* services this app is granted    */
};

/* Look up a catalog entry by name (the manager resolves installed names here). */
const struct app_def *platform_catalog_find(const char *name);

/* The capabilities granted to the currently-running app (0 if none). */
uint32_t manager_caps_of_current(void);

#endif /* PLATFORM_PLATFORM_H_ */
