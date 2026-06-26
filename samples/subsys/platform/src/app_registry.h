/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PERSIST_APP_REGISTRY_H_
#define APP_PERSIST_APP_REGISTRY_H_

#include <zephyr/kernel.h>

/*
 * Installed-app registry, persisted to the filesystem. Installing an app records
 * its metadata so the platform knows, across reboots, what is installed and
 * should be launched at boot. This is the difference between "an app ran once"
 * and "an app is installed on the device".
 */

#define APP_REG_MAX      8
#define APP_NAME_MAX     16

struct app_record {
	char    name[APP_NAME_MAX];
	uint8_t enabled;     /* launch at boot? */
	uint8_t autostart;
	uint32_t install_seq;  /* bumped each install, shows persistence */
};

/* Mount fs and load the registry from flash. Call once at boot. */
int app_registry_load(void);

/* Install (or update) an app record and persist it. */
int app_registry_install(const char *name, bool autostart);

/* Number of installed apps; fetch one by index. */
int app_registry_count(void);
int app_registry_get(int idx, struct app_record *out);

/* Remove an installed app and persist. */
int app_registry_uninstall(const char *name);

#endif /* APP_PERSIST_APP_REGISTRY_H_ */
