/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_REGISTRY_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_REGISTRY_H_

#include <zephyr/kernel.h>

/**
 * @brief Application registry
 * @ingroup app_platform
 * @{
 *
 * The installed-application registry, persisted to a filesystem. Installing an
 * application records its metadata so the platform knows, across reboots, what
 * is installed and should be launched at boot - the difference between "an
 * application ran once" and "an application is installed on the device".
 */

#define APP_REG_MAX  CONFIG_APP_PLATFORM_REGISTRY_MAX_APPS
#define APP_NAME_MAX CONFIG_APP_PLATFORM_REGISTRY_NAME_MAX

/** A persisted record of an installed application. */
struct app_record {
	char     name[APP_NAME_MAX];  /**< application name */
	uint8_t  enabled;             /**< present and runnable */
	uint8_t  autostart;           /**< launch at boot */
	uint32_t install_seq;         /**< bumped on each install */
};

/** Mount the storage and load the registry. Call once at boot. */
int app_registry_load(void);

/** Install (or update) an application record and persist it. */
int app_registry_install(const char *name, bool autostart);

/** Number of installed applications. */
int app_registry_count(void);

/** Fetch an installed application record by index. */
int app_registry_get(int idx, struct app_record *out);

/** Remove an installed application and persist. */
int app_registry_uninstall(const char *name);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_REGISTRY_H_ */
