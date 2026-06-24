/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SERVICES_APP_GROUP_H_
#define APP_SERVICES_APP_GROUP_H_

#include <zephyr/kernel.h>

/*
 * App group: the unit of isolation and lifecycle. An app group is one
 * user-mode application running in its own memory domain, supervised so that
 * if it faults, hangs, or is killed, only that group is torn down - the
 * supervisor and other groups keep running. Groups are drawn from a static
 * pool and their memory domains are REUSED across runs (RISC-V has no
 * k_mem_domain_deinit), so reclaim re-provisions a domain rather than freeing
 * it.
 *
 * Communication rule (enforced by hardware): a user-mode app may not touch
 * kernel globals. It reaches services only through broker syscalls, and the
 * supervisor learns of its state only via granted kobjects (the liveness sem)
 * or the fatal handler.
 */

#define APP_GROUP_MAX        CONFIG_APP_GROUP_MAX
#define APP_GROUP_STACK_SIZE 4096
#define APP_GROUP_HEARTBEAT_MS 8000

enum app_group_state {
	APP_GROUP_FREE = 0,
	APP_GROUP_READY,
	APP_GROUP_RUNNING,
	APP_GROUP_DEAD,
};

struct app_group;

/* Entry point for an app; runs in user mode inside the group's domain. */
typedef void (*app_entry_t)(struct app_group *g, void *arg);

struct app_group {
	uint8_t              id;
	enum app_group_state state;
	const char          *name;

	struct k_mem_domain  domain;       /* reused; never deinit'd on RISC-V */
	struct k_mem_partition *part;      /* this app's private partition     */

	struct k_thread      thread;
	k_thread_stack_t    *stack;

	/* Liveness: the app feeds this; the supervisor watches it. */
	struct k_sem         heartbeat;
	int                  wdt_chan;

	uint32_t             granted_services;  /* bitmask into the registry */

	app_entry_t          entry;
	void                *arg;
};

/**
 * @brief Initialize the app-group subsystem (supervisor side).
 *
 * Sets up the task watchdog and the supervisor thread. Call once at boot.
 */
int app_group_init(void);

/**
 * @brief Create (allocate from the pool) an app group.
 *
 * @param name   Display name.
 * @param part   The app's private memory partition.
 * @param entry  User-mode entry point.
 * @param arg    Opaque argument passed to entry (must be a value or a granted
 *               kobject - never a kernel global the user app would dereference).
 * @return The group, or NULL if the pool is exhausted.
 */
struct app_group *app_group_create(const char *name,
				   struct k_mem_partition *part,
				   app_entry_t entry, void *arg);

/**
 * @brief Grant an app group access to a service (by registry id).
 */
int app_group_grant_service(struct app_group *g, uint32_t service_id);

/**
 * @brief Start the app group's user-mode thread.
 */
int app_group_start(struct app_group *g);

/**
 * @brief Atomically tear down a group: abort its thread, revoke its grants,
 * reclaim its domain partition, and return the slot to the pool. Safe to call
 * from the supervisor thread (not from an ISR).
 */
void app_group_abort(struct app_group *g);

/**
 * @brief Called by a running app to prove liveness (resets its watchdog).
 * This is a thin wrapper over the broker; apps call it via syscall.
 */
void app_group_heartbeat(struct app_group *g);

/**
 * @brief Resolve the app group owning the currently running thread.
 *
 * The manager owns every app thread, so this maps the caller (inside a broker
 * syscall, in supervisor context) back to its group without the user app
 * having to pass an unforgeable kernel pointer. Returns NULL if the current
 * thread is not a managed app.
 */
struct app_group *app_group_current(void);

#endif /* APP_SERVICES_APP_GROUP_H_ */
