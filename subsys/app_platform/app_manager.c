/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Application group lifecycle: launch an application as an isolated user-mode
 * thread in its own memory domain and supervise it so a misbehaving group is
 * reclaimed without affecting the system or the other groups.
 *
 * Liveness follows the model used by mature supervisors (systemd, Erlang/OTP):
 *   - A crash or a clean exit is detected passively, with no cooperation from
 *     the app: a fault routes through the fatal handler, and a normal return
 *     notifies the supervisor from the launch trampoline. The supervisor always
 *     learns of an app's death and reclaims it.
 *   - A hang (the app is alive but not progressing) is the only case that needs
 *     an active check, so a liveness watchdog is opt-in: only an app that asks
 *     to be monitored feeds it; all other apps carry no heartbeat burden.
 */

#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/llext/llext.h>
#ifdef CONFIG_APP_PLATFORM_LOADER
#include <zephyr/app_platform/app_loader.h>
#endif
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

#define MAX_APPS    CONFIG_APP_PLATFORM_MAX_APPS
#define APP_STACK   CONFIG_APP_PLATFORM_APP_STACK_SIZE
#define APP_PRIO    CONFIG_APP_PLATFORM_APP_PRIORITY
#define WDT_TIMEOUT CONFIG_APP_PLATFORM_WDT_TIMEOUT_MS

/* A running app: an isolated user-mode group with its own memory domain. */
struct running_app {
	const char         *name;
	bool                active;
	bool                restart;   /* relaunch (built-in) after a fault */
	bool                monitored; /* opt-in liveness watchdog for hangs */
	struct k_thread     thread;
	struct k_mem_domain domain;
	int                 wdt_chan;  /* -1 unless monitored */
	bool                faulted;   /* reclaimed due to a crash or hang */
	app_entry_t         entry;
	uint32_t            caps;
};

static struct running_app apps[MAX_APPS];
static K_THREAD_STACK_ARRAY_DEFINE(app_stacks, MAX_APPS, APP_STACK);

/* Reclaim requests from the wdt callback / fatal handler -> supervisor thread. */
static struct k_msgq reclaim_q;
static uint8_t reclaim_buf[MAX_APPS];
static struct k_thread sup_thread;
static K_THREAD_STACK_DEFINE(sup_stack, CONFIG_APP_PLATFORM_SUPERVISOR_STACK_SIZE);

static struct running_app *app_of_thread(k_tid_t t)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (apps[i].active && &apps[i].thread == t) {
			return &apps[i];
		}
	}
	return NULL;
}

uint32_t app_platform_caps_current(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	return a ? a->caps : 0U;
}

int app_platform_id_current(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	return a ? (int)(a - apps) : -1;
}

struct k_mem_domain *app_platform_domain(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return NULL;
	}
	return &apps[id].domain;
}

/* A hook a service registers to release per-app resources on reclaim. */
static app_reclaim_hook_t reclaim_hooks[CONFIG_APP_PLATFORM_RECLAIM_HOOKS];

int app_platform_on_reclaim(app_reclaim_hook_t hook)
{
	for (int i = 0; i < CONFIG_APP_PLATFORM_RECLAIM_HOOKS; i++) {
		if (reclaim_hooks[i] == NULL) {
			reclaim_hooks[i] = hook;
			return 0;
		}
	}
	return -ENOSPC;
}

int app_platform_count(void)
{
	return MAX_APPS;
}

int app_platform_status(int id, struct app_platform_status *out)
{
	if (id < 0 || id >= MAX_APPS || out == NULL) {
		return -EINVAL;
	}

	struct running_app *a = &apps[id];

	if (!a->active) {
		return -EAGAIN;
	}

	*out = (struct app_platform_status){
		.name = a->name,
		.active = a->active,
		.monitored = a->monitored,
		.faulted = a->faulted,
	};

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	{
		size_t unused = 0;

		if (k_thread_stack_space_get(&a->thread, &unused) == 0) {
			out->stack_size = a->thread.stack_info.size;
			out->stack_used = out->stack_size - unused;
		}
	}
#endif

#ifdef CONFIG_SCHED_THREAD_USAGE
	{
		k_thread_runtime_stats_t mine, all;

		if (k_thread_runtime_stats_get(&a->thread, &mine) == 0) {
			out->cpu_cycles = mine.total_cycles;
		}
		if (k_thread_runtime_stats_all_get(&all) == 0) {
			out->cpu_total = all.total_cycles;
		}
	}
#endif

	return 0;
}

/* Fault containment: a faulting app is reclaimed, the system survives. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_KERNEL_PANIC) {
		LOG_ERR("kernel panic, halting");
		k_fatal_halt(reason);
	}

	struct running_app *a = app_of_thread(k_current_get());

	if (a) {
		uint8_t id = a - apps;

		LOG_WRN("app '%s' faulted (reason %u) - reclaiming", a->name, reason);
		a->faulted = true;
		(void)k_msgq_put(&reclaim_q, &id, K_NO_WAIT);
	} else {
		k_fatal_halt(reason);
	}
}

/* Only monitored apps have a channel; a timeout here means the app hung. */
static void wdt_cb(int chan, void *user)
{
	ARG_UNUSED(chan);
	struct running_app *a = user;
	uint8_t id = a - apps;

	LOG_WRN("app '%s' is not responding (hang) - reclaiming", a->name);
	a->faulted = true;
	(void)k_msgq_put(&reclaim_q, &id, K_NO_WAIT);
}

void app_platform_heartbeat(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	if (a && a->wdt_chan >= 0) {
		(void)task_wdt_feed(a->wdt_chan);
	}
}

/*
 * Death-watch: one watcher per app, blocked in k_thread_join() until the app
 * thread terminates for any reason (fault, abort or clean return). This is the
 * passive liveness signal - it needs no cooperation from the app - and the
 * equivalent of an OTP link or a parent reaping a child. A fault is also caught
 * earlier by the fatal handler (which flags it for restart); the watcher then
 * just confirms the death. A clean exit is caught only here, with faulted left
 * false, so the app is reclaimed but not relaunched.
 */
static K_THREAD_STACK_ARRAY_DEFINE(watch_stacks, MAX_APPS,
				   CONFIG_APP_PLATFORM_WATCHER_STACK_SIZE);
static struct k_thread watchers[MAX_APPS];

static void death_watch(void *p_app, void *p_id, void *u)
{
	ARG_UNUSED(u);
	struct running_app *a = p_app;
	uint8_t id = (uint8_t)(uintptr_t)p_id;

	k_thread_join(&a->thread, K_FOREVER);
	(void)k_msgq_put(&reclaim_q, &id, K_NO_WAIT);
}

static void reclaim(struct running_app *a)
{
	if (!a->active) {
		return;
	}
	if (a->wdt_chan >= 0) {
		(void)task_wdt_delete(a->wdt_chan);
		a->wdt_chan = -1;
	}
	/* Let services release any per-app resources (e.g. mapped PSRAM). */
	int id = a - apps;

	for (int i = 0; i < CONFIG_APP_PLATFORM_RECLAIM_HOOKS; i++) {
		if (reclaim_hooks[i] != NULL) {
			reclaim_hooks[i](id);
		}
	}

	/* Aborting the app makes its death-watcher's join return; reap it too. */
	k_thread_abort(&a->thread);
	k_thread_abort(&watchers[a - apps]);
	a->active = false;
	LOG_INF("app '%s' reclaimed; system + other apps alive", a->name);
}

int app_platform_stop(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return -EINVAL;
	}
	reclaim(&apps[id]);
	return 0;
}

static int launch(const char *name, app_entry_t entry, uint32_t caps,
		  struct llext *ext, bool restart, bool monitored);

static void supervisor(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	for (;;) {
		uint8_t id;

		if (k_msgq_get(&reclaim_q, &id, K_FOREVER) != 0 || id >= MAX_APPS) {
			continue;
		}

		struct running_app *a = &apps[id];

		if (!a->active) {
			continue;   /* already reclaimed (e.g. double signal) */
		}

		const char *name = a->name;
		app_entry_t entry = a->entry;
		uint32_t caps = a->caps;
		bool monitored = a->monitored;

		/* Restart only a resilient built-in app that crashed or hung. */
		bool restart = a->restart && a->faulted && entry != NULL;

		reclaim(a);

		if (restart) {
			LOG_INF("restarting app '%s' after fault", name);
			(void)launch(name, entry, caps, NULL, true, monitored);
		}
	}
}

static void trampoline(void *entry, void *u1, void *u2)
{
	ARG_UNUSED(u1); ARG_UNUSED(u2);
	((app_entry_t)entry)(NULL);
}

int app_platform_init(void)
{
	k_msgq_init(&reclaim_q, reclaim_buf, sizeof(uint8_t), MAX_APPS);
	if (task_wdt_init(NULL) != 0) {
		return -EIO;
	}
	for (int i = 0; i < MAX_APPS; i++) {
		apps[i].wdt_chan = -1;
	}
	k_thread_create(&sup_thread, sup_stack, K_THREAD_STACK_SIZEOF(sup_stack),
			supervisor, NULL, NULL, NULL,
			CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&sup_thread, "app_supervisor");
	return 0;
}

/* Common launch: find a free slot, set up the domain, start a user-mode thread. */
static int launch(const char *name, app_entry_t entry, uint32_t caps,
		  struct llext *ext, bool restart, bool monitored)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (apps[i].active) {
			continue;
		}
		struct running_app *a = &apps[i];

		a->name = name;
		a->entry = entry;
		a->caps = caps;
		a->restart = restart;
		a->monitored = monitored;
		a->faulted = false;
		a->wdt_chan = -1;
		k_mem_domain_init(&a->domain, 0, NULL);

		if (IS_ENABLED(CONFIG_LLEXT) && ext != NULL) {
#ifdef Z_LIBC_PARTITION_EXISTS
			k_mem_domain_add_partition(&a->domain, &z_libc_partition);
#endif
#ifdef CONFIG_LLEXT
			if (llext_add_domain(ext, &a->domain) != 0) {
				LOG_ERR("could not map '%s' into its domain", name);
				return -ENOSPC;
			}
#endif
		}

		k_tid_t t = k_thread_create(&a->thread, app_stacks[i], APP_STACK,
					    trampoline, (void *)entry, NULL, NULL,
					    APP_PRIO, K_USER | K_INHERIT_PERMS,
					    K_FOREVER);
		k_thread_name_set(t, name);
		k_mem_domain_add_thread(&a->domain, t);
		a->active = true;

		/* Opt-in hang detection: only a monitored app gets a watchdog. */
		if (monitored) {
			a->wdt_chan = task_wdt_add(WDT_TIMEOUT, wdt_cb, a);
		}

		/* A death-watcher gives passive crash/exit detection for every app. */
		k_thread_create(&watchers[i], watch_stacks[i],
				K_THREAD_STACK_SIZEOF(watch_stacks[i]), death_watch,
				a, (void *)(uintptr_t)i, NULL,
				CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY, 0, K_NO_WAIT);

		k_thread_start(t);
		LOG_INF("launched app '%s' (isolated user-mode group %d)", name, i);
		return i;
	}
	return -ENOSPC;
}

int app_platform_launch(const struct app_def *def)
{
	return launch(def->name, def->entry, def->caps, NULL, false, def->monitored);
}

#ifdef CONFIG_LLEXT
int app_platform_launch_llext(const char *name, struct llext *ext,
			      app_entry_t entry, uint32_t caps)
{
#ifdef CONFIG_APP_PLATFORM_LOADER
	/* Refuse any extension that did not pass the verifying loader, so a
	 * launch cannot bypass signature and integrity checks.
	 */
	if (!app_loader_is_verified(ext)) {
		LOG_ERR("refusing to launch '%s': extension not verified", name);
		return -EPERM;
	}
#endif
	return launch(name, entry, caps, ext, false, false);
}
#endif

int app_platform_set_restart(int id, bool restart)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return -EINVAL;
	}
	apps[id].restart = restart;
	return 0;
}
