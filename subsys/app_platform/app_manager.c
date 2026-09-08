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

#include "app_grants.h"
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

#define MAX_APPS    CONFIG_APP_PLATFORM_MAX_APPS
#define MAX_RESTARTS ((uint32_t)CONFIG_APP_PLATFORM_MAX_RESTARTS)
#define APP_STACK   CONFIG_APP_PLATFORM_APP_STACK_SIZE
#define APP_PRIO    CONFIG_APP_PLATFORM_APP_PRIORITY
#define WDT_TIMEOUT CONFIG_APP_PLATFORM_WDT_TIMEOUT_MS

/* Bounded wait for the reaper to release a slot: 100 tries of 2 ms. */
#define CLEANUP_WAIT_TRIES 100
#define CLEANUP_WAIT_MS    2

/* A running app: an isolated user-mode group with its own memory domain. */
/*
 * Application name length, including the terminator. An application's name
 * outlives the call that launched it (the fault handler, the watchdog and the
 * thread name all read it), so the slot keeps its own copy rather than the
 * caller's pointer, which is often a stack local.
 */
#define APP_NAME_LEN 24

struct running_app {
	char                name[APP_NAME_LEN];
	bool                active;    /* slot occupied (running or stopped) */
	bool                running;   /* thread is alive */
	bool                restart;   /* relaunch (built-in) after a fault */
	bool                monitored; /* opt-in liveness watchdog for hangs */
	struct k_thread     thread;
	struct k_mem_domain domain;
	int                 wdt_chan;  /* -1 unless monitored */
	bool                faulted;   /* reclaimed due to a crash or hang */
	uint32_t            restarts;  /* relaunches after a fault, for statistics */
	bool                cleaning;  /* reaper is releasing this slot's resources */
	bool                domain_ready; /* memory domain initialized once per slot */
	app_entry_t         entry;
	app_caps_t          caps;
	size_t              module_bytes; /* loadable-extension footprint, 0 if built-in */
#ifdef CONFIG_LLEXT
	struct llext       *ext;       /* retained so a stopped app can restart */
#endif
};

static struct running_app apps[MAX_APPS];
static K_THREAD_STACK_ARRAY_DEFINE(app_stacks, MAX_APPS, APP_STACK);


static void trampoline(void *entry, void *unused1, void *u2);

/* Reclaim requests from the wdt callback / fatal handler -> supervisor thread. */
static struct k_msgq reclaim_q;
static uint8_t reclaim_buf[MAX_APPS];
static struct k_thread sup_thread;
static K_THREAD_STACK_DEFINE(sup_stack, CONFIG_APP_PLATFORM_SUPERVISOR_STACK_SIZE);

/*
 * Deferred per-application cleanup. The supervisor aborts an application's
 * threads itself (it owns them and that work is bounded and driver-free), then
 * hands the heavy, driver-touching cleanup - service reclaim hooks and module
 * unload - to the reaper-helper thread. Keeping that work off the supervisor
 * means the safety-critical supervisor never blocks on a heap, a service lock
 * or a loader operation.
 */
struct cleanup_req {
	int   id;        /* application slot to release service resources for */
	bool  full;      /* also unload the module captured in ext */
#ifdef CONFIG_LLEXT
	struct llext *ext;
#endif
};

static struct k_msgq cleanup_q;
static char cleanup_buf[MAX_APPS * sizeof(struct cleanup_req)];
static struct k_thread reaper_thread;
static K_THREAD_STACK_DEFINE(reaper_stack, CONFIG_APP_PLATFORM_REAPER_STACK_SIZE);

static struct running_app *app_of_thread(k_tid_t t)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (apps[i].running && &apps[i].thread == t) {
			return &apps[i];
		}
	}
	return NULL;
}

/*
 * The id of the application the current thread belongs to. The identity has to
 * come from state the application cannot reach: an app runs in user mode and
 * k_thread_custom_data_set() is an unrestricted syscall, so anything cached
 * there could be rewritten by the app to borrow another app's capabilities.
 * The thread pointer is owned by the kernel, so it is the identity.
 */
int app_platform_id_current(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	return a ? (int)(a - apps) : -1;
}

app_caps_t app_platform_caps_current(void)
{
	int id = app_platform_id_current();

	return (id >= 0 && id < MAX_APPS) ? apps[id].caps : 0U;
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
		.active = a->running,
		.monitored = a->monitored,
		.faulted = a->faulted,
		.restarts = a->restarts,
		.module_bytes = a->module_bytes,
	};

	/* Thread-level stats are only meaningful while the app is running. */
	if (!a->running) {
		return 0;
	}

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
static char watcher_names[MAX_APPS][16];

/* Give a death-watcher thread a readable name (e.g. "watch:app0"). */
static void name_watcher(int id)
{
	(void)snprintk(watcher_names[id], sizeof(watcher_names[id]), "watch:%d",
		       id);
	k_thread_name_set(&watchers[id], watcher_names[id]);
}

static void death_watch(void *p_app, void *p_id, void *u)
{
	ARG_UNUSED(u);
	struct running_app *a = p_app;
	uint8_t id = (uint8_t)(uintptr_t)p_id;

	k_thread_join(&a->thread, K_FOREVER);
	(void)k_msgq_put(&reclaim_q, &id, K_NO_WAIT);
}

/*
 * The heavy, driver-touching part of reclaim: run each service's reclaim hook
 * (which may free a heap, touch a service lock or reprogram a memory domain) and
 * unload the module. This runs ONLY on the reaper-helper thread, never on the
 * supervisor, so the supervisor stays detached from drivers and service locks.
 */
/* Release the per-resource grants of a slot as it is reclaimed, so the next
 * application to take it inherits nothing. Clearing here rather than at launch
 * keeps the documented order working, where a caller assigns resources to a
 * slot before the application that uses them is started.
 */
static void clear_grants(int id)
{
	if (IS_ENABLED(CONFIG_APP_PLATFORM_GPIO)) {
		app_platform_clear_gpio_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_IO)) {
		app_platform_clear_led_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_PERIPH)) {
		app_platform_clear_periph_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_BUS)) {
		app_platform_clear_bus_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_SERVICES)) {
		app_platform_clear_flash_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_COUNTER)) {
		app_platform_clear_counter_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_DAC)) {
		app_platform_clear_dac_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_EEPROM)) {
		app_platform_clear_eeprom_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_REGULATOR)) {
		app_platform_clear_regulator_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_STEPPER)) {
		app_platform_clear_stepper_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_FS)) {
		app_platform_clear_fs_handles(id);
	}
}

static void run_cleanup(int id, bool full, struct llext *ext)
{
	for (int i = 0; i < CONFIG_APP_PLATFORM_RECLAIM_HOOKS; i++) {
		if (reclaim_hooks[i] != NULL) {
			reclaim_hooks[i](id);
		}
	}
	clear_grants(id);

	if (full) {
#ifdef CONFIG_LLEXT
		if (ext != NULL) {
			/* Take the module's memory back out of the slot's domain
			 * before the extension goes away. The domain outlives the
			 * application so that it is initialized once, so a
			 * partition left behind would be inherited by whichever
			 * application takes the slot next.
			 */
			for (int i = 0; i < LLEXT_MEM_PARTITIONS; i++) {
				if (ext->mem_size[i] != 0) {
					(void)k_mem_domain_remove_partition(
						&apps[id].domain, &ext->mem_parts[i]);
				}
			}
			app_loader_forget(ext);
			llext_unload(&ext);
		}
#else
		ARG_UNUSED(ext);
#endif
		/* Release the slot only after its resources are freed, so the slot
		 * cannot be re-leased to a new application while this cleanup is
		 * still running against the same id.
		 */
		apps[id].active = false;
		apps[id].module_bytes = 0;
		LOG_DBG("app slot %d cleaned up; resources freed", id);
	}

	/* Cleanup for this slot is done; it may be reused or restarted now. */
	apps[id].cleaning = false;
}

/*
 * Wait for the reaper to finish releasing a slot. The reaper runs below the
 * supervisor, so a slot reclaimed from the supervisor is still marked cleaning
 * when control returns; anything that reuses or restarts that slot has to give
 * the reaper a chance to run first. The wait is bounded and touches no driver
 * or service lock, so the supervisor stays detached from both.
 */
static bool wait_cleanup_done(struct running_app *a)
{
	for (int i = 0; i < CLEANUP_WAIT_TRIES && a->cleaning; i++) {
		k_msleep(CLEANUP_WAIT_MS);
	}
	return !a->cleaning;
}

static void reaper(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	for (;;) {
		struct cleanup_req req;

		if (k_msgq_get(&cleanup_q, &req, K_FOREVER) != 0) {
			continue;
		}
#ifdef CONFIG_LLEXT
		run_cleanup(req.id, req.full, req.ext);
#else
		run_cleanup(req.id, req.full, NULL);
#endif
	}
}

/*
 * Stop a running app's thread, then hand its per-app service cleanup and module
 * unload to the reaper-helper thread. Keep the slot and its loaded module when
 * @p full is false (a stop that can be started again); with @p full the slot is
 * released and the module scheduled for unload (a complete reclaim).
 *
 * The supervisor runs only the bounded, driver-free part here (clear flags,
 * delete the watchdog channel, abort the threads it owns) and enqueues the rest.
 */
static void reclaim_ex(struct running_app *a, bool full)
{
	int id = a - apps;
	struct cleanup_req req = { .id = id, .full = full };

	if (!a->running) {
		goto release_slot;
	}

	/*
	 * Clear running before aborting the thread. Aborting wakes the app's
	 * death-watcher, which signals the supervisor to reclaim; the supervisor
	 * skips an app that is no longer running, so an intentional stop is not
	 * mistaken for a crash and turned into a full reclaim.
	 */
	a->running = false;

	if (a->wdt_chan >= 0) {
		(void)task_wdt_delete(a->wdt_chan);
		a->wdt_chan = -1;
	}

	/* Aborting the app makes its death-watcher's join return; reap it too. */
	k_thread_abort(&a->thread);
	k_thread_abort(&watchers[id]);
	LOG_DBG("app '%s' stopped; system + other apps alive", a->name);

release_slot:
	if (full) {
#ifdef CONFIG_LLEXT
		/* Capture the module for the reaper. The slot is freed by the reaper
		 * once cleanup completes, so it is not re-leased mid-cleanup.
		 */
		req.ext = a->ext;
		a->ext = NULL;
#endif
	}

	/* Mark the slot as having cleanup pending, then hand the heavy cleanup
	 * (service hooks + module unload) to the reaper. The slot is not reused
	 * or restarted until the reaper clears this.
	 */
	a->cleaning = true;
	(void)k_msgq_put(&cleanup_q, &req, K_NO_WAIT);
}

static void reclaim(struct running_app *a)
{
	/* Crash/hang path: fully reclaim a faulted app. */
	reclaim_ex(a, true);
}

int app_platform_stop(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active || !apps[id].running) {
		return -EINVAL;
	}
	reclaim_ex(&apps[id], false);
	return 0;
}

#ifdef CONFIG_LLEXT
/* Re-create the user-mode thread of a stopped app in its existing domain. */
static int restart_thread(struct running_app *a)
{
	int id = a - apps;

	/* Wait for any pending reaper cleanup of this slot to finish, so its
	 * per-app service resources are released before the app runs again.
	 */
	if (!wait_cleanup_done(a)) {
		return -EBUSY;
	}

	k_tid_t t = k_thread_create(&a->thread, app_stacks[id], APP_STACK,
				    trampoline, (void *)a->entry, NULL, NULL,
				    APP_PRIO, K_USER | K_INHERIT_PERMS, K_FOREVER);

	k_thread_name_set(t, a->name);
	k_mem_domain_add_thread(&a->domain, t);
	a->faulted = false;
	a->running = true;

	if (a->monitored) {
		a->wdt_chan = task_wdt_add(WDT_TIMEOUT, wdt_cb, a);
	}
	k_thread_create(&watchers[id], watch_stacks[id],
			K_THREAD_STACK_SIZEOF(watch_stacks[id]), death_watch,
			a, (void *)(uintptr_t)id, NULL,
			CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY, 0, K_NO_WAIT);
	name_watcher(id);
	k_thread_start(t);
	LOG_DBG("app '%s' started (group %d)", a->name, id);
	return id;
}

int app_platform_start(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return -EINVAL;
	}
	if (apps[id].running) {
		return -EALREADY;
	}
	if (apps[id].entry == NULL) {
		return -ENOTSUP;
	}
	return restart_thread(&apps[id]);
}

int app_platform_restart(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return -EINVAL;
	}
	if (apps[id].running) {
		reclaim_ex(&apps[id], false);
	}
	return app_platform_start(id);
}
#endif /* CONFIG_LLEXT */

int app_platform_remove(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return -EINVAL;
	}
	reclaim_ex(&apps[id], true);
	return 0;
}


static int launch(const char *name, app_entry_t entry, app_caps_t caps,
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

		if (!a->running) {
			continue;   /* already reclaimed (e.g. double signal) */
		}

		/* Copy the name out: a relaunch writes it back into this same
		 * slot, so passing the slot's own buffer would alias.
		 */
		char name[APP_NAME_LEN];

		(void)strncpy(name, a->name, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';

		app_entry_t entry = a->entry;
		app_caps_t caps = a->caps;
		bool monitored = a->monitored;
		uint32_t restarts = a->restarts;

		/* Restart only a resilient built-in app that crashed or hung,
		 * and only while it stays under the retry budget: an app that
		 * faults in its entry would otherwise respawn without end and
		 * starve the rest of the system.
		 */
		bool restart = a->restart && a->faulted && entry != NULL;

		if (restart && MAX_RESTARTS != 0U && restarts >= MAX_RESTARTS) {
			LOG_ERR("app '%s' faulted %u times, giving up", name, restarts);
			restart = false;
		}

		reclaim(a);

		if (restart) {
			LOG_INF("restarting app '%s' after fault", name);
			/* Let the reaper release this slot before relaunching:
			 * launch() skips a slot that is still being cleaned, so
			 * without the wait every restart leaks a slot and the
			 * app disappears once they run out.
			 */
			if (!wait_cleanup_done(a)) {
				LOG_ERR("app '%s' not restarted: cleanup stuck", name);
				continue;
			}
			int nid = launch(name, entry, caps, NULL, true, monitored);

			if (nid < 0) {
				LOG_ERR("app '%s' could not be restarted", name);
			} else {
				/* The relaunch takes a fresh slot, so the count
				 * has to follow the application, not the slot.
				 */
				apps[nid].restarts = restarts + 1U;
			}
		}
	}
}

static void trampoline(void *entry, void *unused1, void *u2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(u2);
	((app_entry_t)entry)(NULL);
	/* The app returned. Exit through the kernel so a user-mode thread does
	 * not fall off its entry into an undefined return address; the death-
	 * watcher then reclaims the slot.
	 */
	k_thread_abort(k_current_get());
}

int app_platform_init(void)
{
	k_msgq_init(&reclaim_q, reclaim_buf, sizeof(uint8_t), MAX_APPS);
	k_msgq_init(&cleanup_q, cleanup_buf, sizeof(struct cleanup_req), MAX_APPS);
	if (task_wdt_init(NULL) != 0) {
		return -EIO;
	}
	for (int i = 0; i < MAX_APPS; i++) {
		apps[i].wdt_chan = -1;
	}
	k_thread_create(&reaper_thread, reaper_stack,
			K_THREAD_STACK_SIZEOF(reaper_stack), reaper, NULL, NULL,
			NULL, CONFIG_APP_PLATFORM_REAPER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&reaper_thread, "app_reaper");
	k_thread_create(&sup_thread, sup_stack, K_THREAD_STACK_SIZEOF(sup_stack),
			supervisor, NULL, NULL, NULL,
			CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&sup_thread, "app_supervisor");
	return 0;
}

/* Common launch: find a free slot, set up the domain, start a user-mode thread. */
static int launch(const char *name, app_entry_t entry, app_caps_t caps,
		  struct llext *ext, bool restart, bool monitored)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (apps[i].active || apps[i].cleaning) {
			continue;   /* in use or pending reaper cleanup */
		}
		struct running_app *a = &apps[i];

		(void)strncpy(a->name, name, sizeof(a->name) - 1);
		a->name[sizeof(a->name) - 1] = '\0';
		a->entry = entry;
		a->caps = caps;
		a->restart = restart;
		a->monitored = monitored;
		a->faulted = false;
		a->restarts = 0;
		a->wdt_chan = -1;
		a->module_bytes = 0;
#ifdef CONFIG_LLEXT
		a->ext = ext;
#endif
		/* A slot's memory domain is initialized once and reused, because
		 * re-initializing a live domain is a fatal error on some
		 * architectures rather than a fresh start.
		 */
		if (!a->domain_ready) {
			k_mem_domain_init(&a->domain, 0, NULL);
			a->domain_ready = true;
		}

		if (IS_ENABLED(CONFIG_LLEXT) && ext != NULL) {
#ifdef Z_LIBC_PARTITION_EXISTS
			k_mem_domain_add_partition(&a->domain, &z_libc_partition);
#endif
#ifdef CONFIG_LLEXT
			a->module_bytes = ext->alloc_size;
			if (llext_add_domain(ext, &a->domain) != 0) {
				LOG_ERR("could not map '%s' into its domain", name);
				/* The slot never went active, so release the
				 * extension here: the caller only sees the
				 * error and drops its reference.
				 */
				app_loader_forget(ext);
				(void)llext_unload(&ext);
				a->ext = NULL;
				a->module_bytes = 0;
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
		a->running = true;

		/* Opt-in hang detection: only a monitored app gets a watchdog. */
		if (monitored) {
			a->wdt_chan = task_wdt_add(WDT_TIMEOUT, wdt_cb, a);
		}

		/* A death-watcher gives passive crash/exit detection for every app. */
		k_thread_create(&watchers[i], watch_stacks[i],
				K_THREAD_STACK_SIZEOF(watch_stacks[i]), death_watch,
				a, (void *)(uintptr_t)i, NULL,
				CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY, 0, K_NO_WAIT);
		name_watcher(i);

		k_thread_start(t);
		LOG_DBG("launched app '%s' (isolated user-mode group %d)", name, i);
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
			      app_entry_t entry, app_caps_t caps)
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
