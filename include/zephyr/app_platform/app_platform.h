/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_H_

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>

/**
 * @brief Application platform
 * @defgroup app_platform Application platform
 * @ingroup os_services
 * @{
 *
 * A small runtime that launches applications as isolated, supervised groups: an
 * application runs as a user-mode thread in its own memory domain, reaches the
 * system only through application-provided services, and is reclaimed if it
 * misbehaves - without affecting the system or the other applications.
 *
 * The platform provides the mechanism (lifecycle, isolation, supervision,
 * verified loading and persistence). The application provides the policy: the
 * services, the capability meaning of the per-app grant mask, and the catalog of
 * installable applications.
 */

/** How the platform obtains an application's code. */
enum app_source {
	APP_BUILTIN = 0,  /**< compiled into the firmware */
	APP_LOADABLE,     /**< loaded at runtime as an llext */
};

/** An application entry point, run in the application's own user-mode thread. */
typedef void (*app_entry_t)(void *arg);

/**
 * @brief A catalog entry describing an installable application.
 *
 * @p caps is opaque to the platform: it is stored per application and exposed
 * through @ref app_platform_caps_current so the application's services can
 * enforce whatever capability scheme they define.
 */
struct app_def {
	const char     *name;    /**< unique application name */
	enum app_source source;  /**< builtin or loadable */
	app_entry_t     entry;   /**< entry point (APP_BUILTIN) */
	uint32_t        caps;    /**< per-app grant mask (application-defined) */
};

/**
 * @brief Initialize the platform.
 *
 * Sets up supervision and the reclaim path. Call once before launching apps.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int app_platform_init(void);

/**
 * @brief Launch a built-in application as an isolated, supervised group.
 *
 * @param def Catalog entry with a non-NULL @ref app_def.entry.
 * @retval >=0 the application's group id, or negative errno on failure.
 */
int app_platform_launch(const struct app_def *def);

/**
 * @brief Launch a loaded llext application as an isolated, supervised group.
 *
 * The module is mapped into its own memory domain and run as a user-mode thread.
 *
 * @param name Application name.
 * @param ext  Loaded extension (see @ref app_platform_load).
 * @param entry Resolved entry symbol.
 * @param caps Per-app grant mask.
 * @retval >=0 the application's group id, or negative errno on failure.
 */
int app_platform_launch_llext(const char *name, struct llext *ext,
			      app_entry_t entry, uint32_t caps);

/**
 * @brief Feed the calling application's liveness watchdog.
 *
 * An application proves it is alive by calling this; a group that stops feeding
 * is reclaimed.
 */
void app_platform_heartbeat(void);

/**
 * @brief Stop a running application group and free its slot.
 *
 * Aborts the group's thread and releases its watchdog and slot, the same way a
 * faulting group is reclaimed - but on request, e.g. to update or remove it.
 *
 * @param id Group id returned by a launch call.
 * @retval 0 on success, -EINVAL if @p id is not an active group.
 */
int app_platform_stop(int id);

/**
 * @brief Mark a built-in application group to be restarted after a fault.
 *
 * When set, the supervisor relaunches the group (same entry and capabilities)
 * after it is reclaimed for faulting or hanging, so a resilient application can
 * recover. The application is responsible for persisting any state it wants to
 * resume from.
 *
 * @param id Group id returned by a launch call.
 * @param restart Whether to auto-restart after a fault.
 * @retval 0 on success, -EINVAL if @p id is not an active group.
 */
int app_platform_set_restart(int id, bool restart);

/**
 * @brief The grant mask of the currently running application (0 if none).
 *
 * A service uses this to enforce the application's capabilities.
 */
uint32_t app_platform_caps_current(void);

/**
 * @brief The group id of the currently running application (-1 if none).
 */
int app_platform_id_current(void);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_H_ */
