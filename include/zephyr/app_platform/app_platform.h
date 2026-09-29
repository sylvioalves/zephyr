/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_H_

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>

#include <zephyr/app_platform/app_caps.h>

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
 *
 * @p arg reaches the entry point unchanged, also when the application is
 * restarted. The application runs in user mode, so a pointer must refer to
 * memory its domain can read; a plain value, such as an instance number, needs
 * no access.
 *
 * @p cpu_mask restricts the application's threads to a set of CPUs, bit n
 * standing for CPU n; 0 lets them run on any CPU. It needs
 * @kconfig{CONFIG_APP_PLATFORM_CPU_AFFINITY} on an SMP kernel, and a mask with
 * one bit set with @kconfig{CONFIG_SCHED_CPU_MASK_PIN_ONLY}. Without
 * @kconfig{CONFIG_SMP} it is ignored.
 */
struct app_def {
	const char     *name;    /**< unique application name */
	enum app_source source;  /**< builtin or loadable */
	app_entry_t     entry;   /**< entry point (APP_BUILTIN) */
	app_caps_t      caps;    /**< per-app grant mask (application-defined) */
	bool            monitored; /**< opt in to hang detection (see heartbeat) */
	void           *arg;     /**< argument passed to entry, see below */
	size_t          heap_size; /**< per-application heap bytes, 0 for none */
	uint32_t        cpu_mask;  /**< CPUs the application may run on, 0 for any */
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
 * A non-zero @ref app_def.heap_size gives the application a heap region, see
 * @kconfig{CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE}.
 *
 * @param def Catalog entry with a non-NULL @ref app_def.entry.
 * @retval >=0 the application's group id, or negative errno on failure.
 * @retval -EEXIST An application with this name is already running.
 * @retval -E2BIG The heap size is above
 *         @kconfig{CONFIG_APP_PLATFORM_APP_HEAP_MAX_BYTES}.
 * @retval -ENOMEM The heap pool has no room for the heap.
 * @retval -ENOTSUP A heap is declared and the platform has no heap pool, or a
 *         CPU mask is set and @kconfig{CONFIG_APP_PLATFORM_CPU_AFFINITY} is
 *         disabled.
 * @retval -EINVAL The CPU mask names a CPU the system does not have, or more
 *         than one with @kconfig{CONFIG_SCHED_CPU_MASK_PIN_ONLY}.
 */
int app_platform_launch(const struct app_def *def);

/**
 * @brief Launch a loaded llext application as an isolated, supervised group.
 *
 * The module is mapped into its own memory domain and run as a user-mode thread.
 * It gets a heap of the size its signed manifest declares, see
 * @ref app_loader_heap_size.
 *
 * @param name Application name.
 * @param ext  Loaded extension (see @ref app_platform_load).
 * @param entry Resolved entry symbol.
 * @param arg  Argument passed to @p entry, as for @ref app_def.arg.
 * @param caps Per-app grant mask.
 * @retval >=0 the application's group id, or negative errno on failure.
 * @retval -EEXIST An application with this name is already running.
 * @retval -E2BIG The heap size is above
 *         @kconfig{CONFIG_APP_PLATFORM_APP_HEAP_MAX_BYTES}.
 * @retval -ENOMEM The heap pool has no room for the heap.
 * @retval -ENOTSUP A heap is declared and the platform has no heap pool.
 */
int app_platform_launch_llext(const char *name, struct llext *ext,
			      app_entry_t entry, void *arg, app_caps_t caps);

/** Options for @ref app_platform_launch_llext_opts. */
struct app_launch_opts {
	uint32_t cpu_mask; /**< CPUs the application may run on, as @ref app_def.cpu_mask */
};

/**
 * @brief Launch a loaded llext application with launch options.
 *
 * As @ref app_platform_launch_llext, with the options in @p opts applied.
 *
 * @param name Application name.
 * @param ext  Loaded extension (see @ref app_platform_load).
 * @param entry Resolved entry symbol.
 * @param arg  Argument passed to @p entry, as for @ref app_def.arg.
 * @param caps Per-app grant mask.
 * @param opts Launch options, or NULL for the defaults of
 *        @ref app_platform_launch_llext.
 * @retval >=0 the application's group id, or negative errno on failure.
 * @retval -EINVAL The CPU mask names a CPU the system does not have, or more
 *         than one with @kconfig{CONFIG_SCHED_CPU_MASK_PIN_ONLY}.
 * @retval -ENOTSUP The CPU mask is not 0 and
 *         @kconfig{CONFIG_APP_PLATFORM_CPU_AFFINITY} is disabled.
 *
 * The other return values are those of @ref app_platform_launch_llext.
 */
int app_platform_launch_llext_opts(const char *name, struct llext *ext, app_entry_t entry,
				   void *arg, app_caps_t caps, const struct app_launch_opts *opts);

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
 * A group stopped inside a platform service keeps running until it releases
 * its service locks, for at most @kconfig{CONFIG_APP_PLATFORM_STOP_WAIT_MS}, so
 * no lock is left held by a dead thread. Its resources are released after that.
 *
 * @param id Group id returned by a launch call.
 * @retval 0 on success, -EINVAL if @p id is not a running group.
 */
int app_platform_stop(int id);

/**
 * @brief Ask an application group to exit, then remove it.
 *
 * Flags the group (see @ref app_platform_stop_requested), posts
 * @ref APP_EVENT_STOP to its event queue when the event service is enabled, and
 * waits up to @p grace_ms for it to return from its entry point. A group still
 * running then is removed as by @ref app_platform_remove. The caller blocks for
 * up to @p grace_ms.
 *
 * @param id Group id.
 * @param grace_ms Time the group has to exit on its own.
 * @retval 0 The group exited on its own.
 * @retval -ETIMEDOUT The group did not exit in time and was removed.
 * @retval -EINVAL @p id is not a running group.
 */
int app_platform_request_stop(int id, uint32_t grace_ms);

/**
 * @brief Report whether the calling application was asked to exit.
 *
 * Set by @ref app_platform_request_stop. An application that sees it can end
 * cleanly by returning from its entry point.
 *
 * @retval true A stop was requested.
 * @retval false No stop was requested, or the caller is not an application.
 */
bool app_platform_stop_requested(void);

/**
 * @brief Start a previously stopped application group.
 *
 * Re-creates the group's user-mode thread in its existing memory domain from
 * the loaded module. Only valid on a group that is loaded but not running.
 *
 * @param id Group id.
 * @retval 0 on success (returns the id), -EALREADY if already running,
 *         -EINVAL if not a loaded group, -ENOTSUP if it cannot be restarted,
 *         -EBUSY if the previous stop has not completed.
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
 * be started again without loading it anew. A group inside a platform service
 * is stopped as described for @ref app_platform_stop.
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
 * resume from. A loadable application cannot be marked: reclaiming it unloads
 * the module its entry point lives in.
 *
 * @param id Group id returned by a launch call.
 * @param restart Whether to auto-restart after a fault.
 * @retval 0 Success.
 * @retval -EINVAL @p id is not an active group.
 * @retval -ENOTSUP @p restart is set and the group is a loadable application.
 */
int app_platform_set_restart(int id, bool restart);

/**
 * @brief The grant mask of the currently running application (0 if none).
 *
 * A service uses this to enforce the application's capabilities.
 *
 * @return The capability mask, or zero when the caller is not an application.
 */
app_caps_t app_platform_caps_current(void);

/**
 * @brief The group id of the currently running application.
 *
 * The identity follows the calling thread, which the kernel owns, so an
 * application cannot present itself as a different one.
 *
 * @return The group id, or -1 when the caller is not an application.
 */
int app_platform_id_current(void);

/**
 * @brief The memory domain of an application group.
 *
 * Lets a service map memory into an application's domain - for example a region
 * of external RAM the application accesses directly. Without
 * @kconfig{CONFIG_USERSPACE} applications have no memory domain.
 *
 * @return The domain, or NULL if the group is not active or the kernel has no
 *         user mode.
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

/**
 * @brief Take over a fatal error raised by an application thread.
 *
 * Queues the faulting application for reclaim when the current thread belongs
 * to one. The platform calls this from its own k_sys_fatal_error_handler()
 * unless @kconfig{CONFIG_APP_PLATFORM_FATAL_HANDLER} is disabled, in which case
 * the handler that replaces it must call this first and return when it reports
 * true.
 *
 * @param reason Reason code passed to k_sys_fatal_error_handler().
 * @param esf    Exception stack frame passed to k_sys_fatal_error_handler().
 * @retval true The fault belongs to an application, which will be reclaimed.
 * @retval false A kernel panic or a fault outside any application.
 */
bool app_platform_handle_fatal_error(unsigned int reason, const struct arch_esf *esf);

/**
 * @brief Enable or mute per-application log output (app_svc_log).
 *
 * Muting lets a console operator silence the application stream while typing,
 * without stopping the applications.
 */
void app_platform_log_enable(bool en);

/**
 * @brief Whether per-application log output is currently enabled.
 *
 * @return True while application log output is enabled.
 */
bool app_platform_log_is_enabled(void);

/** A snapshot of one application group's identity and resource use. */
struct app_platform_status {
	const char *name;          /**< application name (NULL if the slot is free) */
	bool        active;        /**< the group is running */
	bool        monitored;     /**< opted in to hang detection */
	bool        faulted;       /**< last reclaim was due to a crash or hang */
	uint32_t    restarts;      /**< times the supervisor relaunched this app */
	size_t      stack_size;    /**< usable stack bytes */
	size_t      stack_used;    /**< stack high-water mark in bytes */
	size_t      module_bytes;  /**< loadable-extension footprint (0 if built-in) */
	size_t      heap_bytes;    /**< heap region size (0 without a heap) */
	uint64_t    cpu_cycles;    /**< non-idle cycles attributed to this app */
	uint64_t    cpu_total;     /**< non-idle cycles across the whole system */
};

/**
 * @brief Number of application group slots.
 *
 * Group ids run from 0 to this minus one; iterate to inspect every slot.
 *
 * @return The number of slots, which is a build-time constant.
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
