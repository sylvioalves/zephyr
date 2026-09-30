/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Application group lifecycle: launch an application as an isolated user-mode
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
#include <zephyr/app_platform/app_services.h>

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/llext/llext.h>
#ifdef CONFIG_APP_PLATFORM_LOADER
#include <zephyr/app_platform/app_loader.h>
#endif

#include "app_events.h"
#include "app_grants.h"
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

#define MAX_RESTARTS ((uint32_t)CONFIG_APP_PLATFORM_MAX_RESTARTS)
#define APP_STACK   CONFIG_APP_PLATFORM_APP_STACK_SIZE
#define APP_PRIO    CONFIG_APP_PLATFORM_APP_PRIORITY
#define WDT_TIMEOUT CONFIG_APP_PLATFORM_WDT_TIMEOUT_MS

/* Bounded wait for the reaper to release a slot: 100 tries of 2 ms. */
#define CLEANUP_WAIT_TRIES 100
#define CLEANUP_WAIT_MS    2

/* Service locks one application thread can hold at once. */
#define SVC_HOLD_MAX 4

/* Step of a service lock wait, so a waiter notices a lock that breaks. */
#define SVC_LOCK_STEP K_MSEC(50)

/* Application name length, including the terminator. An application's name
 * outlives the call that launched it (the fault handler, the watchdog and the
 * thread name all read it), so the slot keeps its own copy rather than the
 * caller's pointer, which is often a stack local.
 */
#define APP_NAME_LEN APP_PLATFORM_NAME_LEN

/* Threads of one application; the first is the one its entry point runs on. */
#define APP_WORKERS CONFIG_APP_PLATFORM_APP_THREADS
#define APP_THREADS (1 + APP_WORKERS)

/* One thread of an application and the service locks it holds. */
struct app_thread {
	struct k_thread     thread;
	bool                live;      /* created and not yet reaped */
	uint8_t             gen;       /* worker generation, part of its handle */
	uint8_t             svc_holds; /* service locks this thread holds */
	struct svc_lock    *svc_held[SVC_HOLD_MAX];
};

/* A running app: a supervised thread group, isolated in user mode with its own
 * memory domain when the kernel provides user mode.
 */
struct running_app {
	char                name[APP_NAME_LEN];
	bool                active;    /* slot occupied (running or stopped) */
	bool                running;   /* main thread is alive */
	bool                restart;   /* relaunch (built-in) after a fault */
	bool                monitored; /* opt-in liveness watchdog for hangs */
	struct app_thread   threads[APP_THREADS];
#ifdef CONFIG_USERSPACE
	struct k_mem_domain domain;
#endif
	int                 wdt_chan;  /* -1 unless monitored */
	bool                faulted;   /* reclaimed due to a crash or hang */
	uint32_t            restarts;  /* relaunches after a fault, for statistics */
	bool                cleaning;  /* reaper is releasing this slot's resources */
	bool                stop_requested; /* asked to exit by app_platform_request_stop */
	bool                stopping;  /* stop requested, deferred while a thread holds a lock */
	bool                domain_ready; /* memory domain initialized once per slot */
	uint8_t             gen;       /* instance of the slot, bumped at each start */
	app_entry_t         entry;
	void               *arg;       /* passed to entry */
	app_caps_t          caps;
	size_t              module_bytes; /* loadable-extension footprint, 0 if built-in */
	size_t              heap_size; /* heap bytes declared at launch */
	uint32_t            cpu_mask;  /* CPUs the threads may run on, 0 for any */
	app_configure_t     configure; /* called before each start, may be NULL */
	void               *configure_user;
#ifdef CONFIG_LLEXT
	struct llext       *ext;       /* retained so a stopped app can restart */
#endif
};

static struct running_app apps[MAX_APPS];
static K_THREAD_STACK_ARRAY_DEFINE(app_stacks, MAX_APPS, APP_STACK);
#if APP_WORKERS > 0
static K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, MAX_APPS * APP_WORKERS,
				   CONFIG_APP_PLATFORM_APP_THREAD_STACK_SIZE);
static SVC_LOCK_DEFINE(spawn_lock);
#endif

static void trampoline(void *entry, void *arg, void *u2);

static int check_cpu_mask(uint32_t cpu_mask)
{
	if (cpu_mask == 0U || !IS_ENABLED(CONFIG_SMP)) {
		return 0;
	}
	if (!IS_ENABLED(CONFIG_APP_PLATFORM_CPU_AFFINITY)) {
		return -ENOTSUP;
	}
	if (((uint64_t)cpu_mask >> arch_num_cpus()) != 0U ||
	    (IS_ENABLED(CONFIG_SCHED_CPU_MASK_PIN_ONLY) && !IS_POWER_OF_TWO(cpu_mask))) {
		return -EINVAL;
	}
	return 0;
}

/* Restrict a created, not yet started application thread to the app's CPUs. */
static void pin_thread(const struct running_app *a, k_tid_t t)
{
#ifdef CONFIG_APP_PLATFORM_CPU_AFFINITY
	if (a->cpu_mask == 0U) {
		return;
	}
	if (IS_POWER_OF_TWO(a->cpu_mask)) {
		(void)k_thread_cpu_pin(t, find_lsb_set(a->cpu_mask) - 1);
		return;
	}
	(void)k_thread_cpu_mask_clear(t);
	for (unsigned int cpu = 0; cpu < arch_num_cpus(); cpu++) {
		if ((a->cpu_mask & BIT(cpu)) != 0U) {
			(void)k_thread_cpu_mask_enable(t, cpu);
		}
	}
#else
	ARG_UNUSED(a);
	ARG_UNUSED(t);
#endif
}

/* Reclaim requests from the wdt callback / fatal handler -> supervisor thread. */
struct reclaim_req {
	uint8_t id;
	uint8_t gen; /* instance the request is about */
};

static struct k_msgq reclaim_q;
static struct reclaim_req reclaim_buf[MAX_APPS];
static struct k_thread sup_thread;
static K_THREAD_STACK_DEFINE(sup_stack, CONFIG_APP_PLATFORM_SUPERVISOR_STACK_SIZE);

/* Deferred per-application cleanup. The supervisor aborts an application's
 * threads itself (it owns them and that work is bounded and driver-free), then
 * hands the heavy, driver-touching cleanup - service reclaim hooks and module
 * unload - to the reaper-helper thread. Keeping that work off the supervisor
 * means the safety-critical supervisor never blocks on a heap, a service lock
 * or a loader operation.
 */
struct cleanup_req {
	int   id;        /* application slot to release service resources for */
	bool  full;      /* also unload the module captured in ext */
	bool  deferred;  /* an app thread still holds a service lock */
#ifdef CONFIG_LLEXT
	struct llext *ext;
#endif
};

static struct k_msgq cleanup_q;
static struct k_spinlock hold_lock;
static char cleanup_buf[MAX_APPS * sizeof(struct cleanup_req)];
static struct k_thread reaper_thread;
static K_THREAD_STACK_DEFINE(reaper_stack, CONFIG_APP_PLATFORM_REAPER_STACK_SIZE);

static struct app_thread *thread_of(struct running_app *a, k_tid_t t)
{
	for (int j = 0; j < APP_THREADS; j++) {
		if (&a->threads[j].thread == t) {
			return &a->threads[j];
		}
	}
	return NULL;
}

static struct running_app *app_of_thread(k_tid_t t)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (apps[i].running && thread_of(&apps[i], t) != NULL) {
			return &apps[i];
		}
	}
	return NULL;
}

/* The id of the application the current thread belongs to. The identity has to
 * come from state the application cannot reach: an app runs in user mode and
 * k_thread_custom_data_set() is an unrestricted syscall, so anything cached
 * there could be rewritten by the app to borrow another app's capabilities.
 * The thread pointer is owned by the kernel, so it is the identity.
 */
static struct running_app *app_of_self(struct app_thread **thread)
{
	k_tid_t self = k_current_get();

	for (int i = 0; i < MAX_APPS; i++) {
		struct app_thread *t = apps[i].active ? thread_of(&apps[i], self) : NULL;

		if (t != NULL) {
			*thread = t;
			return &apps[i];
		}
	}
	return NULL;
}

static void drop_hold(struct running_app *a, struct app_thread *t, struct svc_lock *lock)
{
	k_spinlock_key_t key = k_spin_lock(&hold_lock);
	bool stop;

	for (int i = t->svc_holds - 1; i >= 0; i--) {
		if (t->svc_held[i] == lock) {
			t->svc_held[i] = t->svc_held[t->svc_holds - 1];
			break;
		}
	}
	t->svc_holds--;
	stop = a->stopping && t->svc_holds == 0U;
	k_spin_unlock(&hold_lock, key);
	if (stop) {
		k_thread_abort(k_current_get());
	}
}

int app_service_lock(struct svc_lock *lock, k_timeout_t timeout)
{
	struct app_thread *t = NULL;
	struct running_app *a = app_of_self(&t);
	k_timepoint_t end = sys_timepoint_calc(timeout);
	k_timepoint_t step;
	int rc;

	if (a != NULL) {
		k_spinlock_key_t key = k_spin_lock(&hold_lock);

		if (a->stopping && t->svc_holds == 0U) {
			k_spin_unlock(&hold_lock, key);
			k_thread_abort(k_current_get());
		}
		if (t->svc_holds == SVC_HOLD_MAX) {
			k_spin_unlock(&hold_lock, key);
			return -EBUSY;
		}
		t->svc_held[t->svc_holds++] = lock;
		k_spin_unlock(&hold_lock, key);
	}

	do {
		if (atomic_get(&lock->broken) != 0) {
			rc = -EIO;
			break;
		}
		step = sys_timepoint_calc(SVC_LOCK_STEP);
		if (sys_timepoint_cmp(end, step) < 0) {
			step = end;
		}
		rc = k_mutex_lock(&lock->mutex, sys_timepoint_timeout(step));
	} while (rc != 0 && !sys_timepoint_expired(end));

	if (rc != 0 && a != NULL) {
		drop_hold(a, t, lock);
	}
	return rc;
}

int app_service_unlock(struct svc_lock *lock)
{
	struct app_thread *t = NULL;
	struct running_app *a = app_of_self(&t);
	int rc = k_mutex_unlock(&lock->mutex);

	if (a != NULL) {
		drop_hold(a, t, lock);
	}
	return rc;
}

int app_service_lock_reclaim(struct svc_lock *lock, k_timeout_t timeout)
{
	if (k_current_get() == &reaper_thread && atomic_get(&lock->broken) != 0) {
		return SVC_LOCK_FORCED;
	}
	return app_service_lock(lock, timeout);
}

#ifdef CONFIG_APP_PLATFORM_TEST_FAULT_POINTS
__weak void app_platform_test_point(int point)
{
	ARG_UNUSED(point);
}
#endif

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

static struct k_mem_domain *slot_domain(struct running_app *a)
{
#ifdef CONFIG_USERSPACE
	return &a->domain;
#else
	ARG_UNUSED(a);
	return NULL;
#endif
}

struct k_mem_domain *app_platform_domain(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return NULL;
	}
#ifdef CONFIG_USERSPACE
	return &apps[id].domain;
#else
	return NULL;
#endif
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
		.heap_bytes = app_platform_heap_bytes(id),
	};

	/* Thread-level stats are only meaningful while the app is running. */
	if (!a->running) {
		return 0;
	}

#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
	{
		size_t unused = 0;

		if (k_thread_stack_space_get(&a->threads[0].thread, &unused) == 0) {
			out->stack_size = a->threads[0].thread.stack_info.size;
			out->stack_used = out->stack_size - unused;
		}
	}
#endif

#ifdef CONFIG_SCHED_THREAD_USAGE
	{
		k_thread_runtime_stats_t mine, all;

		if (k_thread_runtime_stats_get(&a->threads[0].thread, &mine) == 0) {
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
bool app_platform_handle_fatal_error(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_KERNEL_PANIC) {
		return false;
	}

	struct running_app *a = NULL;

	/* A thread of an application being stopped still belongs to it. */
	for (int i = 0; i < MAX_APPS && a == NULL; i++) {
		if (apps[i].active && thread_of(&apps[i], k_current_get()) != NULL) {
			a = &apps[i];
		}
	}
	if (a == NULL) {
		return false;
	}

	struct reclaim_req req = { .id = a - apps, .gen = a->gen };

	LOG_WRN("app '%s' faulted (reason %u) - reclaiming", a->name, reason);
	a->faulted = true;
	(void)k_msgq_put(&reclaim_q, &req, K_NO_WAIT);
	return true;
}

#ifdef CONFIG_APP_PLATFORM_FATAL_HANDLER
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	if (!app_platform_handle_fatal_error(reason, esf)) {
		LOG_ERR("fatal error %u outside any application, halting", reason);
		k_fatal_halt(reason);
	}
}
#endif

/* Only monitored apps have a channel; a timeout here means the app hung. */
static void wdt_cb(int chan, void *user)
{
	ARG_UNUSED(chan);
	struct running_app *a = user;
	struct reclaim_req req = { .id = a - apps, .gen = a->gen };

	LOG_WRN("app '%s' is not responding (hang) - reclaiming", a->name);
	a->faulted = true;
	(void)k_msgq_put(&reclaim_q, &req, K_NO_WAIT);
}

void app_platform_heartbeat(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	if (a && a->wdt_chan >= 0) {
		(void)task_wdt_feed(a->wdt_chan);
	}
}

/* Death-watch: one watcher per app, blocked in k_thread_join() until the app
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

static void death_watch(void *p_app, void *p_id, void *p_gen)
{
	struct running_app *a = p_app;
	struct reclaim_req req = {
		.id = (uint8_t)(uintptr_t)p_id,
		.gen = (uint8_t)(uintptr_t)p_gen,
	};

	k_thread_join(&a->threads[0].thread, K_FOREVER);
	(void)k_msgq_put(&reclaim_q, &req, K_NO_WAIT);
}

/* The heavy, driver-touching part of reclaim: run each service's reclaim hook
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
	if (IS_ENABLED(CONFIG_APP_PLATFORM_EVENTS)) {
		app_events_clear(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_IO)) {
		app_platform_clear_led_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_RGB)) {
		app_platform_clear_rgb_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_DISPLAY)) {
		app_platform_clear_display_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_PERIPH)) {
		app_platform_clear_periph_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_BUS)) {
		app_platform_clear_bus_grants(id);
	}
	if (IS_ENABLED(CONFIG_APP_PLATFORM_SERVICES)) {
		app_platform_clear_flash_grants(id);
		app_platform_clear_ipc(id);
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
	app_platform_clear_sems(id);
}

#if defined(CONFIG_USERSPACE) && defined(CONFIG_LLEXT)
/* Remove the partitions of @p ext that are in @p dom. */
static void unmap_module(struct k_mem_domain *dom, struct llext *ext)
{
	for (int i = 0; i < LLEXT_MEM_PARTITIONS; i++) {
		struct k_mem_partition *part = &ext->mem_parts[i];

		if (part->size == 0U) {
			continue;
		}
		for (int j = 0; j < CONFIG_MAX_DOMAIN_PARTITIONS; j++) {
			if (dom->partitions[j].size == part->size &&
			    dom->partitions[j].start == part->start) {
				(void)k_mem_domain_remove_partition(dom, part);
				break;
			}
		}
	}
}
#endif

static void run_cleanup(int id, bool full, struct llext *ext)
{
	if (full) {
		app_platform_heap_release(id, slot_domain(&apps[id]));
	}
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
#ifdef CONFIG_USERSPACE
			unmap_module(&apps[id].domain, ext);
#endif
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
		apps[id].heap_size = 0;
		LOG_DBG("app slot %d cleaned up; resources freed", id);
	}

	/* Cleanup for this slot is done; it may be reused or restarted now. */
	apps[id].cleaning = false;
}

/* Wait for the reaper to finish releasing a slot. The reaper runs below the
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

		struct running_app *a = &apps[req.id];
		k_timepoint_t end = sys_timepoint_calc(K_MSEC(CONFIG_APP_PLATFORM_STOP_WAIT_MS));
		k_spinlock_key_t key;
		uint8_t broken = 0U;

		/* A stopped thread inside a service ends itself when it releases its
		 * last service lock, so no lock is left owned by a dead thread.
		 */
		for (int j = 0; j < APP_THREADS; j++) {
			struct app_thread *t = &a->threads[j];

			if (req.deferred && t->live && t->svc_holds != 0U) {
				(void)k_thread_join(&t->thread, sys_timepoint_timeout(end));
			}
		}
		for (int j = 0; j < APP_THREADS; j++) {
			if (a->threads[j].live) {
				k_thread_abort(&a->threads[j].thread);
			}
		}
		key = k_spin_lock(&hold_lock);
		for (int j = 0; j < APP_THREADS; j++) {
			struct app_thread *t = &a->threads[j];

			/* A hold is recorded before the lock is taken. */
			for (uint8_t i = 0U; i < t->svc_holds; i++) {
				struct svc_lock *lock = t->svc_held[i];

				if (lock->mutex.owner == &t->thread) {
					atomic_set(&lock->broken, 1);
					broken++;
				}
			}
			t->svc_holds = 0U;
			t->live = false;
		}
		a->stopping = false;
		k_spin_unlock(&hold_lock, key);
		if (broken != 0U) {
			LOG_ERR("app '%s' stopped holding %u service lock(s), service disabled",
				a->name, broken);
		}

#ifdef CONFIG_LLEXT
		run_cleanup(req.id, req.full, req.ext);
#else
		run_cleanup(req.id, req.full, NULL);
#endif
	}
}

/* Stop a running app's thread, then hand its per-app service cleanup and module
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
	bool holding[APP_THREADS];
	k_spinlock_key_t key;

	if (!a->running) {
		goto release_slot;
	}

	/* Clear running before aborting the thread. Aborting wakes the app's
	 * death-watcher, which signals the supervisor to reclaim; the supervisor
	 * skips an app that is no longer running, so an intentional stop is not
	 * mistaken for a crash and turned into a full reclaim.
	 */
	a->running = false;

	if (a->wdt_chan >= 0) {
		(void)task_wdt_delete(a->wdt_chan);
		a->wdt_chan = -1;
	}

	/* A thread inside a service keeps running until it releases its last
	 * service lock, and ends itself there.
	 */
	key = k_spin_lock(&hold_lock);
	a->stopping = true;
	for (int j = 0; j < APP_THREADS; j++) {
		holding[j] = a->threads[j].live && a->threads[j].svc_holds != 0U;
		req.deferred = req.deferred || holding[j];
	}
	k_spin_unlock(&hold_lock, key);

	for (int j = 0; j < APP_THREADS; j++) {
		if (a->threads[j].live && !holding[j]) {
			k_thread_abort(&a->threads[j].thread);
		}
	}
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

/* Create and start the main thread and the death-watcher of an active slot. */
static int start_app(struct running_app *a)
{
	int id = a - apps;
	k_tid_t t = k_thread_create(&a->threads[0].thread, app_stacks[id], APP_STACK,
				    trampoline, (void *)a->entry, a->arg, NULL,
				    APP_PRIO, K_USER, K_FOREVER);

	a->threads[0].live = true;

	k_thread_name_set(t, a->name);
	pin_thread(a, t);
#ifdef CONFIG_USERSPACE
	k_mem_domain_add_thread(&a->domain, t);
#endif
	a->faulted = false;
	a->stop_requested = false;
	a->running = true;

	/* Opt-in hang detection: only a monitored app gets a watchdog. */
	if (a->monitored) {
		a->wdt_chan = task_wdt_add(WDT_TIMEOUT, wdt_cb, a);
	}

	/* A death-watcher gives passive crash/exit detection for every app. */
	a->gen++;
	k_thread_create(&watchers[id], watch_stacks[id],
			K_THREAD_STACK_SIZEOF(watch_stacks[id]), death_watch,
			a, (void *)(uintptr_t)id, (void *)(uintptr_t)a->gen,
			CONFIG_APP_PLATFORM_SUPERVISOR_PRIORITY, 0, K_NO_WAIT);
	name_watcher(id);
	k_thread_start(t);
	LOG_DBG("app '%s' started (group %d)", a->name, id);
	return id;
}

/* Run the configure hook of a slot whose cleanup is complete, right before it
 * starts.
 */
static int prepare_start(struct running_app *a)
{
	int rc = 0;

	if (a->configure != NULL) {
		rc = a->configure(a - apps, a->configure_user);
	}
	if (rc != 0) {
		LOG_WRN("app '%s' not started: configure failed (%d)", a->name, rc);
		return rc < 0 ? rc : -ECANCELED;
	}
	return 0;
}

/* Re-create the user-mode thread of a stopped app in its existing domain. */
static int restart_thread(struct running_app *a)
{
	int rc;

	/* Wait for any pending reaper cleanup of this slot to finish, so its
	 * per-app service resources are released before the app runs again.
	 */
	if (!wait_cleanup_done(a)) {
		return -EBUSY;
	}
	app_platform_heap_reset(a - apps);
	rc = prepare_start(a);
	if (rc != 0) {
		return rc;
	}
	return start_app(a);
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

int app_platform_request_stop(int id, uint32_t grace_ms)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active || !apps[id].running) {
		return -EINVAL;
	}

	struct running_app *a = &apps[id];
	struct app_event evt = { .kind = APP_EVENT_STOP };

	a->stop_requested = true;
	app_events_post(id, &evt);
	if (k_thread_join(&a->threads[0].thread, K_MSEC(grace_ms)) == 0) {
		return 0;
	}
	LOG_WRN("app '%s' did not exit within %u ms, removing it", a->name, grace_ms);
	reclaim_ex(a, true);
	return -ETIMEDOUT;
}

bool app_platform_stop_requested(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	return a != NULL && a->stop_requested;
}

int app_platform_remove(int id)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return -EINVAL;
	}
	reclaim_ex(&apps[id], true);
	return 0;
}

static int launch(const struct app_def *def, struct llext *ext, int prev);

static void supervisor(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	for (;;) {
		struct reclaim_req req;

		if (k_msgq_get(&reclaim_q, &req, K_FOREVER) != 0 || req.id >= MAX_APPS) {
			continue;
		}

		struct running_app *a = &apps[req.id];

		/* Skip a slot already reclaimed (e.g. double signal) or a request
		 * about an earlier instance than the one now in the slot.
		 */
		if (!a->running || req.gen != a->gen) {
			continue;
		}

		/* Copy the name out: a relaunch writes it back into this same
		 * slot, so passing the slot's own buffer would alias.
		 */
		char name[APP_NAME_LEN];

		(void)strncpy(name, a->name, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';

		const struct app_def def = {
			.name = name,
			.source = APP_BUILTIN,
			.entry = a->entry,
			.caps = a->caps,
			.monitored = a->monitored,
			.arg = a->arg,
			.heap_size = a->heap_size,
			.cpu_mask = a->cpu_mask,
			.configure = a->configure,
			.configure_user = a->configure_user,
		};
		uint32_t restarts = a->restarts;

		/* Restart only a resilient built-in app that crashed or hung,
		 * and only while it stays under the retry budget: an app that
		 * faults in its entry would otherwise respawn without end and
		 * starve the rest of the system.
		 */
		bool restart = a->restart && a->faulted && def.entry != NULL;

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
			int nid = launch(&def, NULL, req.id);

			if (nid < 0) {
				LOG_ERR("app '%s' could not be restarted (%d)", name, nid);
			} else {
				apps[nid].restarts = restarts + 1U;
			}
		}
	}
}

static void trampoline(void *entry, void *arg, void *u2)
{
	ARG_UNUSED(u2);
	((app_entry_t)entry)(arg);
	/* The app returned. Exit through the kernel so a user-mode thread does
	 * not fall off its entry into an undefined return address; the death-
	 * watcher then reclaims the slot.
	 */
	k_thread_abort(k_current_get());
}

#if APP_WORKERS > 0
/* A worker slot never started or whose thread has ended. */
static struct app_thread *free_worker(struct running_app *a, int *slot)
{
	for (int j = 1; j < APP_THREADS; j++) {
		struct app_thread *t = &a->threads[j];

		if (!t->live || k_thread_join(&t->thread, K_NO_WAIT) == 0) {
			*slot = j;
			return t;
		}
	}
	return NULL;
}
#endif

int z_impl_app_svc_thread_spawn(app_thread_entry_t entry, void *arg, int prio_offset)
{
#if APP_WORKERS > 0
	struct app_thread *self = NULL;
	struct running_app *a = app_of_self(&self);
	struct app_thread *t;
	char name[APP_NAME_LEN + 4];
	k_tid_t tid;
	int slot = 0;
	int id;
	int rc;

	if (a == NULL || !app_has_cap(APP_CAP_THREAD)) {
		return -EPERM;
	}
	if (entry == NULL || prio_offset < 0) {
		return -EINVAL;
	}

	/* Keeps a stop deferred until the new worker is recorded. */
	rc = app_service_lock(&spawn_lock, K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	if (a->stopping) {
		rc = -ESRCH;
		goto out;
	}
	t = free_worker(a, &slot);
	if (t == NULL) {
		rc = -EAGAIN;
		goto out;
	}

	id = a - apps;
	t->gen++;
	t->svc_holds = 0U;
	tid = k_thread_create(&t->thread, worker_stacks[id * APP_WORKERS + slot - 1],
			      K_THREAD_STACK_SIZEOF(worker_stacks[0]), trampoline,
			      (void *)entry, arg, NULL,
			      MIN(APP_PRIO + prio_offset, K_LOWEST_APPLICATION_THREAD_PRIO),
			      K_USER, K_FOREVER);
	(void)snprintk(name, sizeof(name), "%s.w%d", a->name, slot);
	k_thread_name_set(tid, name);
	pin_thread(a, tid);
#ifdef CONFIG_USERSPACE
	k_mem_domain_add_thread(&a->domain, tid);
#endif
	t->live = true;
	k_thread_start(tid);
	rc = ((int)t->gen << 4) | slot;
out:
	(void)app_service_unlock(&spawn_lock);
	return rc;
#else
	ARG_UNUSED(entry);
	ARG_UNUSED(arg);
	ARG_UNUSED(prio_offset);
	return -ENOTSUP;
#endif
}

int z_impl_app_svc_thread_join(int handle, int32_t timeout_ms)
{
	struct app_thread *self = NULL;
	struct running_app *a = app_of_self(&self);
	int slot = handle & 0xF;
	struct app_thread *t;

	if (a == NULL || !app_has_cap(APP_CAP_THREAD)) {
		return -EPERM;
	}
	if (handle <= 0 || (handle >> 4) > UINT8_MAX || slot < 1 || slot >= APP_THREADS) {
		return -EINVAL;
	}
	t = &a->threads[slot];
	if (!t->live || t->gen != (uint8_t)(handle >> 4)) {
		return -ESRCH;
	}
	if (t == self) {
		return -EDEADLK;
	}
	return k_thread_join(&t->thread, timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms));
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_thread_spawn(app_thread_entry_t entry, void *arg, int prio_offset)
{
	return z_impl_app_svc_thread_spawn(entry, arg, prio_offset);
}
#include <zephyr/syscalls/app_svc_thread_spawn_mrsh.c>

static inline int z_vrfy_app_svc_thread_join(int handle, int32_t timeout_ms)
{
	return z_impl_app_svc_thread_join(handle, timeout_ms);
}
#include <zephyr/syscalls/app_svc_thread_join_mrsh.c>
#endif

int app_platform_init(void)
{
	k_msgq_init(&reclaim_q, (char *)reclaim_buf, sizeof(struct reclaim_req), MAX_APPS);
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

#ifdef CONFIG_USERSPACE
/* Remove every partition but the C library's from an unused slot domain. */
static void drop_partitions(struct k_mem_domain *dom)
{
	for (int j = 0; j < CONFIG_MAX_DOMAIN_PARTITIONS; j++) {
		struct k_mem_partition part = dom->partitions[j];

		if (part.size == 0U) {
			continue;
		}
#ifdef Z_LIBC_PARTITION_EXISTS
		if (part.start == z_libc_partition.start && part.size == z_libc_partition.size) {
			continue;
		}
#endif
		(void)k_mem_domain_remove_partition(dom, &part);
	}
}
#endif

/* Common launch: find a free slot, set up the domain and, unless the definition
 * asks for a stopped launch, start a user-mode thread. A relaunch after a fault
 * passes the slot it held in @p prev and takes it again when it is free.
 */
static int launch(const struct app_def *def, struct llext *ext, int prev)
{
	const char *name = def->name;
	size_t heap_size = def->heap_size;
	int err = check_cpu_mask(def->cpu_mask);

	if (err != 0) {
		return err;
	}

	for (int i = 0; i < MAX_APPS; i++) {
		if (apps[i].active && strncmp(apps[i].name, name, APP_NAME_LEN) == 0) {
			return -EEXIST;
		}
	}

	for (int n = 0; n < MAX_APPS; n++) {
		int i = (prev >= 0) ? (prev + n) % MAX_APPS : n;

		if (apps[i].active || apps[i].cleaning) {
			continue;   /* in use or pending reaper cleanup */
		}
		struct running_app *a = &apps[i];
		int rc;

		(void)strncpy(a->name, name, sizeof(a->name) - 1);
		a->name[sizeof(a->name) - 1] = '\0';
		a->entry = def->entry;
		a->arg = def->arg;
		a->caps = def->caps;
		a->restart = prev >= 0;
		a->monitored = def->monitored;
		a->faulted = false;
		a->stop_requested = false;
		a->restarts = 0;
		a->wdt_chan = -1;
		a->module_bytes = 0;
		a->heap_size = heap_size;
		a->cpu_mask = def->cpu_mask;
		a->configure = def->configure;
		a->configure_user = def->configure_user;
#ifdef CONFIG_LLEXT
		a->ext = ext;
#endif
		/* A slot's memory domain is initialized once and reused, because
		 * re-initializing a live domain is a fatal error on some
		 * architectures rather than a fresh start. A new application
		 * gets it back with the C library partition only, while a
		 * relaunch after a fault keeps what the slot had.
		 */
#ifdef CONFIG_USERSPACE
		if (!a->domain_ready) {
			k_mem_domain_init(&a->domain, 0, NULL);
#ifdef Z_LIBC_PARTITION_EXISTS
			k_mem_domain_add_partition(&a->domain, &z_libc_partition);
#endif
			a->domain_ready = true;
		} else if (i != prev) {
			drop_partitions(&a->domain);
		}
#endif

		rc = app_platform_heap_carve(i, slot_domain(a), heap_size);
		if (rc != 0) {
			LOG_ERR("no %zu-byte heap for '%s': %d", heap_size, name, rc);
#ifdef CONFIG_LLEXT
			a->ext = NULL;
#endif
			a->heap_size = 0;
			return rc;
		}

#ifdef CONFIG_LLEXT
		if (ext != NULL) {
			a->module_bytes = ext->alloc_size;
		}
#endif
#if defined(CONFIG_USERSPACE) && defined(CONFIG_LLEXT)
		if (ext != NULL && llext_add_domain(ext, &a->domain) != 0) {
			LOG_ERR("could not map '%s' into its domain", name);
			/* The slot never went active, so release the extension
			 * here: the caller only sees the error and drops its
			 * reference.
			 */
			unmap_module(&a->domain, ext);
			app_platform_heap_release(i, slot_domain(a));
			app_loader_forget(ext);
			(void)llext_unload(&ext);
			a->ext = NULL;
			a->module_bytes = 0;
			a->heap_size = 0;
			return -ENOSPC;
		}
#endif

		a->active = true;
		if (def->stopped) {
			LOG_DBG("launched app '%s' stopped (group %d)", name, i);
			return i;
		}
		rc = prepare_start(a);
		if (rc != 0) {
			/* A relaunch after a fault stays stopped in its slot. A new
			 * launch is released; its caller keeps the extension.
			 */
			if (prev < 0) {
#ifdef CONFIG_LLEXT
#ifdef CONFIG_USERSPACE
				if (ext != NULL) {
					unmap_module(&a->domain, ext);
				}
#endif
				a->ext = NULL;
#endif
				reclaim_ex(a, true);
			}
			return rc;
		}
		return start_app(a);
	}
	return -ENOSPC;
}

int app_platform_launch(const struct app_def *def)
{
	return launch(def, NULL, -1);
}

#ifdef CONFIG_LLEXT
int app_platform_launch_llext(const char *name, struct llext *ext,
			      app_entry_t entry, void *arg, app_caps_t caps)
{
	return app_platform_launch_llext_opts(name, ext, entry, arg, caps, NULL);
}

int app_platform_launch_llext_opts(const char *name, struct llext *ext, app_entry_t entry,
				   void *arg, app_caps_t caps, const struct app_launch_opts *opts)
{
	struct app_def def = {
		.name = name,
		.source = APP_LOADABLE,
		.entry = entry,
		.caps = caps,
		.arg = arg,
	};

	if (opts != NULL) {
		def.cpu_mask = opts->cpu_mask;
		def.stopped = opts->stopped;
		def.configure = opts->configure;
		def.configure_user = opts->configure_user;
	}

#ifdef CONFIG_APP_PLATFORM_LOADER
	/* Refuse any extension that did not pass the verifying loader, so a
	 * launch cannot bypass signature and integrity checks.
	 */
	if (!app_loader_is_verified(ext)) {
		LOG_ERR("refusing to launch '%s': extension not verified", name);
		return -EPERM;
	}
	def.heap_size = app_loader_heap_size(ext);
#endif
	return launch(&def, ext, -1);
}
#endif

int app_platform_set_restart(int id, bool restart)
{
	if (id < 0 || id >= MAX_APPS || !apps[id].active) {
		return -EINVAL;
	}
#ifdef CONFIG_LLEXT
	if (restart && apps[id].ext != NULL) {
		return -ENOTSUP;
	}
#endif
	apps[id].restart = restart;
	return 0;
}
