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
	bool            monitored; /**< opt in to hang detection (see heartbeat) */
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
 * Only meaningful for an application launched with @ref app_def.monitored set:
 * such an application opts in to hang detection and must call this periodically,
 * and is reclaimed if it stops. Applications that are not monitored need not
 * call this - their crash or exit is still detected passively - so ordinary
 * applications carry no heartbeat burden. A no-op when called by an unmonitored
 * application.
 */
void app_platform_heartbeat(void);

/**
 * @brief Stop a running application group, keeping it loaded.
 *
 * Aborts the group's thread and releases its watchdog and per-app service
 * resources, but keeps the slot and the loaded module so the group can be
 * started again with @ref app_platform_start. Use @ref app_platform_remove to
 * fully reclaim it.
 *
 * @param id Group id returned by a launch call.
 * @retval 0 on success, -EINVAL if @p id is not a running group.
 */
int app_platform_stop(int id);

/**
 * @brief Start a previously stopped application group.
 *
 * Re-creates the group's user-mode thread in its existing memory domain from
 * the loaded module. Only valid on a group that is loaded but not running.
 *
 * @param id Group id.
 * @retval 0 on success (returns the id), -EALREADY if already running,
 *         -EINVAL if not a loaded group, -ENOTSUP if it cannot be restarted.
 */
int app_platform_start(int id);

/**
 * @brief Restart an application group (stop then start).
 *
 * @param id Group id.
 * @retval id on success, negative errno on failure.
 */
int app_platform_restart(int id);

/**
 * @brief Remove an application group and free everything.
 *
 * Stops the group if running, unloads its module, and frees the slot. Unlike
 * @ref app_platform_stop this releases the module memory, so the group cannot
 * be started again without loading it anew.
 *
 * @param id Group id.
 * @retval 0 on success, -EINVAL if @p id is not a loaded group.
 */
int app_platform_remove(int id);

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

/**
 * @brief The memory domain of an application group, or NULL if not active.
 *
 * Lets a service map memory into an application's domain - for example a region
 * of external RAM the application accesses directly.
 */
struct k_mem_domain *app_platform_domain(int id);

/** A hook called with a group id when that group is reclaimed. */
typedef void (*app_reclaim_hook_t)(int id);

/**
 * @brief Register a hook to release per-application resources on reclaim.
 *
 * A service that holds resources for an application (a mapped region, a handle)
 * registers a hook so they are freed when the application is reclaimed.
 *
 * @param hook Called with the reclaimed group id.
 * @retval 0 on success, -ENOSPC if the hook table is full.
 */
int app_platform_on_reclaim(app_reclaim_hook_t hook);

/** A snapshot of one application group's identity and resource use. */
struct app_platform_status {
	const char *name;          /**< application name (NULL if the slot is free) */
	bool        active;        /**< the group is running */
	bool        monitored;     /**< opted in to hang detection */
	bool        faulted;       /**< last reclaim was due to a crash or hang */
	size_t      stack_size;    /**< usable stack bytes */
	size_t      stack_used;    /**< stack high-water mark in bytes */
	size_t      module_bytes;  /**< loadable-extension footprint (0 if built-in) */
	uint64_t    cpu_cycles;    /**< non-idle cycles attributed to this app */
	uint64_t    cpu_total;     /**< non-idle cycles across the whole system */
};

/**
 * @brief Number of application group slots.
 *
 * Group ids run from 0 to this minus one; iterate to inspect every slot.
 */
int app_platform_count(void);

/**
 * @brief Read a snapshot of one application group's status.
 *
 * Reports the group's stack high-water mark and the CPU cycles charged to it,
 * so a privileged inspector can show per-application memory and CPU use without
 * the application's cooperation. Stack figures need @kconfig{CONFIG_THREAD_STACK_INFO};
 * CPU figures need @kconfig{CONFIG_THREAD_RUNTIME_STATS} (otherwise reported 0).
 *
 * @param id  Group id in the range [0, @ref app_platform_count).
 * @param out Filled with the snapshot on success.
 * @retval 0 on success, -EINVAL for a bad @p id or NULL @p out, -EAGAIN if the
 *         slot is not active.
 */
int app_platform_status(int id, struct app_platform_status *out);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_H_ */
