/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Worker threads and semaphores of one application. */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#define WORKERS  CONFIG_APP_PLATFORM_APP_THREADS
#define SEMS     CONFIG_APP_PLATFORM_APP_SEMS
#define THR_CAPS (APP_CAP_LOG | APP_CAP_THREAD)

K_APPMEM_PARTITION_DEFINE(thr_part);

struct thr_report {
	int rc[8];
	int handle;
	int sem;
	int prio;
	bool done;
};

K_APP_BMEM(thr_part) static struct thr_report rep;
K_APP_BMEM(thr_part) static volatile uint32_t spins;

static int slot_of(const char *name)
{
	for (int i = 0; i < app_platform_count(); i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0 && st.name != NULL &&
		    strcmp(st.name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool wait_slot_free(const char *name, int ms)
{
	for (int i = 0; i < ms / 5; i++) {
		if (slot_of(name) < 0) {
			return true;
		}
		k_msleep(5);
	}
	return slot_of(name) < 0;
}

static int launch(const char *name, app_entry_t entry, app_caps_t caps)
{
	const struct app_def def = {
		.name = name,
		.source = APP_BUILTIN,
		.entry = entry,
		.caps = caps,
	};
	int id = app_platform_launch(&def);

#ifdef CONFIG_USERSPACE
	if (id >= 0) {
		struct k_mem_domain *dom = app_platform_domain(id);
		bool found = false;

		for (int i = 0; i < CONFIG_MAX_DOMAIN_PARTITIONS; i++) {
			if (dom->partitions[i].start == thr_part.start &&
			    dom->partitions[i].size == thr_part.size) {
				found = true;
			}
		}
		if (!found) {
			k_mem_domain_add_partition(dom, &thr_part);
		}
	}
#endif
	return id;
}

static void run_to_end(const char *name, app_entry_t entry, app_caps_t caps)
{
	zassert_true(launch(name, entry, caps) >= 0, "%s launch failed", name);
	zassert_true(wait_slot_free(name, 3000), "%s never ended", name);
}

static bool spins_stopped(void)
{
	uint32_t before = spins;

	k_msleep(50);
	return spins == before;
}

static void count_worker(void *arg)
{
	ARG_UNUSED(arg);
	for (int i = 0; i < 100; i++) {
		spins++;
	}
}

static void spin_worker(void *arg)
{
	ARG_UNUSED(arg);
	for (;;) {
		spins++;
		k_msleep(2);
	}
}

static void sem_worker(void *arg)
{
	(void)app_svc_sem_take((int)(uintptr_t)arg, -1);
	rep.done = true;
}

static void self_join_worker(void *arg)
{
	ARG_UNUSED(arg);
	while (rep.handle == 0) {
		k_msleep(1);
	}
	rep.rc[1] = app_svc_thread_join(rep.handle, 0);
}

static void prio_worker(void *arg)
{
	ARG_UNUSED(arg);
	rep.prio = k_thread_priority_get(k_current_get());
}

static void sha_worker(void *arg)
{
	uint8_t in[4] = {1, 2, 3, 4};
	uint8_t out[32];

	ARG_UNUSED(arg);
	rep.rc[1] = app_svc_sha256(in, sizeof(in), out);
}

static void crash_worker(void *arg)
{
	volatile uint32_t *p = NULL;

	ARG_UNUSED(arg);
	*p = 0xdead;
}

static void spawn_join_app(void *arg)
{
	int h = app_svc_thread_spawn(count_worker, NULL, 0);

	ARG_UNUSED(arg);
	rep.rc[0] = h;
	rep.rc[1] = h > 0 ? app_svc_thread_join(h, -1) : h;
}

ZTEST(threads, test_spawn_and_join)
{
	run_to_end("t_join", spawn_join_app, THR_CAPS);
	zassert_true(rep.rc[0] > 0, "spawn rc %d", rep.rc[0]);
	zassert_equal(rep.rc[1], 0, "join rc %d", rep.rc[1]);
	zassert_equal(spins, 100U, "worker ran %u of 100 steps", spins);
}

static void no_cap_app(void *arg)
{
	ARG_UNUSED(arg);
	rep.rc[0] = app_svc_thread_spawn(count_worker, NULL, 0);
	rep.rc[1] = app_svc_thread_join(1, 0);
	rep.rc[2] = app_svc_sem_init(0, 1);
	rep.rc[3] = app_svc_sem_give(0);
	rep.rc[4] = app_svc_sem_take(0, 0);
}

ZTEST(threads, test_calls_need_capability)
{
	run_to_end("t_nocap", no_cap_app, APP_CAP_LOG);
	for (int i = 0; i < 5; i++) {
		zassert_equal(rep.rc[i], -EPERM, "call %d rc %d", i, rep.rc[i]);
	}
}

static void bad_args_app(void *arg)
{
	ARG_UNUSED(arg);
	rep.rc[0] = app_svc_thread_spawn(NULL, NULL, 0);
	rep.rc[1] = app_svc_thread_spawn(count_worker, NULL, -1);
	rep.rc[2] = app_svc_thread_join(0, 0);
	rep.rc[3] = app_svc_thread_join(WORKERS + 1, 0);
	rep.rc[4] = app_svc_thread_join(-5, 0);
}

ZTEST(threads, test_bad_arguments)
{
	run_to_end("t_args", bad_args_app, THR_CAPS);
	zassert_equal(rep.rc[0], -EINVAL, "NULL entry rc %d", rep.rc[0]);
	zassert_equal(rep.rc[1], -EINVAL, "negative priority rc %d", rep.rc[1]);
	for (int i = 2; i < 5; i++) {
		zassert_equal(rep.rc[i], -EINVAL, "bad handle %d rc %d", i, rep.rc[i]);
	}
}

static void limit_app(void *arg)
{
	int h[WORKERS];
	int s = app_svc_sem_init(0, WORKERS);

	ARG_UNUSED(arg);
	for (int i = 0; i < WORKERS; i++) {
		h[i] = app_svc_thread_spawn(sem_worker, (void *)(uintptr_t)s, 0);
	}
	rep.rc[0] = app_svc_thread_spawn(count_worker, NULL, 0);
	for (int i = 0; i < WORKERS; i++) {
		(void)app_svc_sem_give(s);
		(void)app_svc_thread_join(h[i], -1);
	}
	rep.rc[1] = app_svc_thread_spawn(count_worker, NULL, 0);
	rep.rc[2] = app_svc_thread_join(rep.rc[1], -1);
	rep.rc[3] = app_svc_thread_join(h[0], 0);
}

ZTEST(threads, test_worker_limit_and_stale_handle)
{
	run_to_end("t_limit", limit_app, THR_CAPS);
	zassert_equal(rep.rc[0], -EAGAIN, "spawn past the limit rc %d", rep.rc[0]);
	zassert_true(rep.rc[1] > 0, "spawn after workers ended rc %d", rep.rc[1]);
	zassert_equal(rep.rc[2], 0, "join rc %d", rep.rc[2]);
	zassert_equal(rep.rc[3], -ESRCH, "stale handle rc %d", rep.rc[3]);
}

static void self_join_app(void *arg)
{
	ARG_UNUSED(arg);
	rep.handle = app_svc_thread_spawn(self_join_worker, NULL, 0);
	rep.rc[0] = app_svc_thread_join(rep.handle, -1);
}

ZTEST(threads, test_worker_cannot_join_itself)
{
	run_to_end("t_self", self_join_app, THR_CAPS);
	zassert_true(rep.handle > 0, "spawn rc %d", rep.handle);
	zassert_equal(rep.rc[0], 0, "join rc %d", rep.rc[0]);
	zassert_equal(rep.rc[1], -EDEADLK, "self join rc %d", rep.rc[1]);
}

static void prio_app(void *arg)
{
	ARG_UNUSED(arg);
	rep.rc[0] = app_svc_thread_spawn(prio_worker, NULL, 1000);
	rep.rc[1] = app_svc_thread_join(rep.rc[0], -1);
}

ZTEST(threads, test_priority_is_clamped)
{
	run_to_end("t_prio", prio_app, THR_CAPS);
	zassert_true(rep.rc[0] > 0, "spawn rc %d", rep.rc[0]);
	zassert_equal(rep.rc[1], 0, "join rc %d", rep.rc[1]);
	zassert_equal(rep.prio, K_LOWEST_APPLICATION_THREAD_PRIO, "priority %d", rep.prio);
}

static void sha_app(void *arg)
{
	ARG_UNUSED(arg);
	rep.rc[0] = app_svc_thread_spawn(sha_worker, NULL, 0);
	(void)app_svc_thread_join(rep.rc[0], -1);
}

ZTEST(threads, test_worker_uses_application_capabilities)
{
	run_to_end("t_sha", sha_app, THR_CAPS | APP_CAP_CRYPTO);
	zassert_equal(rep.rc[1], 0, "worker sha256 rc %d", rep.rc[1]);

	memset(&rep, 0, sizeof(rep));
	run_to_end("t_sha", sha_app, THR_CAPS);
	zassert_equal(rep.rc[1], -EPERM, "worker without the capability rc %d", rep.rc[1]);
}

static void crash_app(void *arg)
{
	ARG_UNUSED(arg);
	(void)app_svc_thread_spawn(crash_worker, NULL, 0);
	for (;;) {
		spins++;
		k_msleep(2);
	}
}

ZTEST(threads, test_worker_fault_reclaims_application)
{
	zassert_true(launch("t_crash", crash_app, THR_CAPS) >= 0);
	zassert_true(wait_slot_free("t_crash", 3000), "faulting worker left the app running");
	zassert_true(spins_stopped(), "entry thread survived the reclaim");
}

static void leave_workers_app(void *arg)
{
	ARG_UNUSED(arg);
	(void)app_svc_thread_spawn(spin_worker, NULL, 0);
	k_msleep(20);
}

ZTEST(threads, test_entry_exit_ends_workers)
{
	run_to_end("t_exit", leave_workers_app, THR_CAPS);
	zassert_true(spins > 0U, "worker never ran");
	zassert_true(spins_stopped(), "worker outlived the application");
}

static void stop_app(void *arg)
{
	ARG_UNUSED(arg);
	for (int i = 0; i < WORKERS; i++) {
		(void)app_svc_thread_spawn(spin_worker, NULL, 0);
	}
	for (;;) {
		k_msleep(10);
	}
}

ZTEST(threads, test_stop_ends_workers)
{
	int id = launch("t_stop", stop_app, THR_CAPS);

	zassert_true(id >= 0);
	k_msleep(30);
	zassert_true(spins > 0U, "workers never ran");
	zassert_equal(app_platform_stop(id), 0);
	k_msleep(20);
	zassert_true(spins_stopped(), "workers kept running after a stop");
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("t_stop", 3000));
}

static void sem_app(void *arg)
{
	ARG_UNUSED(arg);
	rep.sem = app_svc_sem_init(0, 1);
	rep.rc[0] = app_svc_sem_take(rep.sem, 0);
	rep.rc[1] = app_svc_sem_take(rep.sem, 10);
	rep.rc[2] = app_svc_thread_spawn(sem_worker, (void *)(uintptr_t)rep.sem, 0);
	k_msleep(20);
	rep.rc[3] = rep.done ? 1 : 0;
	rep.rc[4] = app_svc_sem_give(rep.sem);
	rep.rc[5] = app_svc_thread_join(rep.rc[2], -1);
}

ZTEST(threads, test_semaphore_handoff)
{
	run_to_end("t_sem", sem_app, THR_CAPS);
	zassert_true(rep.sem >= 0, "sem init rc %d", rep.sem);
	zassert_equal(rep.rc[0], -EBUSY, "poll rc %d", rep.rc[0]);
	zassert_equal(rep.rc[1], -EAGAIN, "timed take rc %d", rep.rc[1]);
	zassert_true(rep.rc[2] > 0, "spawn rc %d", rep.rc[2]);
	zassert_equal(rep.rc[3], 0, "worker passed an empty semaphore");
	zassert_equal(rep.rc[4], 0, "give rc %d", rep.rc[4]);
	zassert_equal(rep.rc[5], 0, "join rc %d", rep.rc[5]);
	zassert_true(rep.done, "worker never took the semaphore");
}

static void sem_errors_app(void *arg)
{
	int last = 0;

	ARG_UNUSED(arg);
	rep.rc[0] = app_svc_sem_init(0, 0);
	rep.rc[1] = app_svc_sem_init(2, 1);
	for (int i = 0; i < SEMS; i++) {
		last = app_svc_sem_init(0, 1);
	}
	rep.rc[2] = last;
	rep.rc[3] = app_svc_sem_init(0, 1);
	rep.rc[4] = app_svc_sem_give(SEMS);
	rep.rc[5] = app_svc_sem_take(-1, 0);
}

ZTEST(threads, test_semaphore_errors)
{
	run_to_end("t_semerr", sem_errors_app, THR_CAPS);
	zassert_equal(rep.rc[0], -EINVAL, "limit 0 rc %d", rep.rc[0]);
	zassert_equal(rep.rc[1], -EINVAL, "initial above limit rc %d", rep.rc[1]);
	zassert_equal(rep.rc[2], SEMS - 1, "last semaphore rc %d", rep.rc[2]);
	zassert_equal(rep.rc[3], -ENOMEM, "semaphore past the pool rc %d", rep.rc[3]);
	zassert_equal(rep.rc[4], -EINVAL, "give bad handle rc %d", rep.rc[4]);
	zassert_equal(rep.rc[5], -EINVAL, "take bad handle rc %d", rep.rc[5]);
}

static void threads_before(void *f)
{
	ARG_UNUSED(f);
	memset(&rep, 0, sizeof(rep));
	spins = 0U;
}

ZTEST_SUITE(threads, NULL, NULL, threads_before, NULL, NULL);
