/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* CPU affinity of applications on an SMP kernel. */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#define CPUS     CONFIG_MP_MAX_NUM_CPUS
#define WORKERS  CONFIG_APP_PLATFORM_APP_THREADS
#define PIN_CAPS (APP_CAP_LOG | APP_CAP_THREAD)
#define RUN_MS   200
#define GONE_MS  200

/* Switch-ins of the watched application's threads, per CPU. */
static const char *volatile watch_name;
static atomic_t main_runs[CPUS];
static atomic_t worker_runs[CPUS];

void sys_trace_thread_switched_in_user(void)
{
	struct _cpu *cpu = arch_curr_cpu();
	const char *want = watch_name;
	const char *name;
	size_t len;

	if (want == NULL) {
		return;
	}
	name = k_thread_name_get(cpu->current);
	len = strlen(want);
	if (name == NULL || strncmp(name, want, len) != 0) {
		return;
	}
	if (name[len] == '\0') {
		atomic_inc(&main_runs[cpu->id]);
	} else if (name[len] == '.') {
		atomic_inc(&worker_runs[cpu->id]);
	}
}

static void watch(const char *name)
{
	watch_name = NULL;
	for (int i = 0; i < CPUS; i++) {
		atomic_clear(&main_runs[i]);
		atomic_clear(&worker_runs[i]);
	}
	watch_name = name;
}

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

/* Wait until a restarting application stays reclaimed. */
static bool wait_given_up(const char *name, int ms)
{
	int gone = 0;

	for (int i = 0; i < ms / 5 && gone < GONE_MS / 5; i++) {
		gone = slot_of(name) < 0 ? gone + 1 : 0;
		k_msleep(5);
	}
	return gone >= GONE_MS / 5;
}

static int launch_on(const char *name, app_entry_t entry, uint32_t cpu_mask)
{
	const struct app_def def = {
		.name = name,
		.source = APP_BUILTIN,
		.entry = entry,
		.caps = PIN_CAPS,
		.cpu_mask = cpu_mask,
	};

	return app_platform_launch(&def);
}

static void busy_worker(void *arg)
{
	volatile uint32_t n = 0;

	ARG_UNUSED(arg);
	for (;;) {
		n++;
		if ((n % 4096U) == 0U) {
			k_yield();
		}
	}
}

static void busy_app(void *arg)
{
	ARG_UNUSED(arg);
	for (int i = 0; i < WORKERS; i++) {
		(void)app_svc_thread_spawn(busy_worker, NULL, 0);
	}
	busy_worker(NULL);
}

/* Run busy_app for RUN_MS restricted to cpu_mask, then remove it. */
static void run_busy(const char *name, uint32_t cpu_mask)
{
	int id;

	watch(name);
	id = launch_on(name, busy_app, cpu_mask);
	zassert_true(id >= 0, "launch rc %d", id);
	k_msleep(RUN_MS);
	watch_name = NULL;
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free(name, 3000), "%s was not reclaimed", name);
}

static void expect_only_on(int cpu)
{
	for (int i = 0; i < arch_num_cpus(); i++) {
		if (i == cpu) {
			zassert_true(atomic_get(&main_runs[i]) > 0, "entry never ran on CPU %d", i);
			zassert_true(atomic_get(&worker_runs[i]) > 0, "workers never ran on CPU %d",
				     i);
		} else {
			zassert_equal(atomic_get(&main_runs[i]), 0, "entry ran on CPU %d", i);
			zassert_equal(atomic_get(&worker_runs[i]), 0, "a worker ran on CPU %d", i);
		}
	}
}

ZTEST(smp, test_pinned_app_and_workers_stay_on_cpu)
{
	for (int cpu = 0; cpu < arch_num_cpus(); cpu++) {
		run_busy("t_pin", BIT(cpu));
		expect_only_on(cpu);
	}
}

ZTEST(smp, test_unpinned_app_runs_anywhere)
{
	run_busy("t_any", 0U);
	for (int i = 0; i < arch_num_cpus(); i++) {
		zassert_true(atomic_get(&main_runs[i]) + atomic_get(&worker_runs[i]) > 0,
			     "unpinned application never ran on CPU %d", i);
	}
}

ZTEST(smp, test_stop_start_keeps_pin)
{
	int cpu = arch_num_cpus() - 1;
	int id = launch_on("t_restart", busy_app, BIT(cpu));

	zassert_true(id >= 0, "launch rc %d", id);
	k_msleep(RUN_MS / 4);
	zassert_equal(app_platform_stop(id), 0);
	watch("t_restart");
	zassert_equal(app_platform_start(id), id);
	k_msleep(RUN_MS);
	watch_name = NULL;
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("t_restart", 3000));
	expect_only_on(cpu);
}

static void crash_app(void *arg)
{
	volatile uint32_t *p = NULL;

	ARG_UNUSED(arg);
	k_busy_wait(1000);
	*p = 0xdead;
}

ZTEST(smp, test_fault_on_pinned_cpu_is_reclaimed)
{
	int cpu = arch_num_cpus() - 1;
	uint32_t runs;
	int id;

	watch("t_crash");
	id = launch_on("t_crash", crash_app, BIT(cpu));
	zassert_true(id >= 0, "launch rc %d", id);
	zassert_equal(app_platform_set_restart(id, true), 0);
	zassert_true(wait_given_up("t_crash", 10000), "faulting application was not reclaimed");
	watch_name = NULL;

	runs = atomic_get(&main_runs[cpu]);
	zassert_true(runs > CONFIG_APP_PLATFORM_MAX_RESTARTS, "restarted %u times", runs);
	for (int i = 0; i < arch_num_cpus(); i++) {
		if (i != cpu) {
			zassert_equal(atomic_get(&main_runs[i]), 0, "restart ran on CPU %d", i);
		}
	}

	run_busy("t_after", BIT(cpu));
	expect_only_on(cpu);
}

ZTEST(smp, test_cpu_mask_checked)
{
	zassert_equal(launch_on("t_bad", busy_app, BIT(arch_num_cpus())), -EINVAL);
	zassert_equal(slot_of("t_bad"), -1);
}

#if defined(CONFIG_USERSPACE) && defined(Z_LIBC_PARTITION_EXISTS)
/* Handshake in memory every application domain maps from the start. */
K_APP_BMEM(z_libc_partition) static volatile uint32_t worker_spinning;
K_APP_BMEM(z_libc_partition) static volatile uint32_t partition_added;
K_APP_BMEM(z_libc_partition) static volatile uint32_t access_done;

K_APPMEM_PARTITION_DEFINE(late_part);
K_APP_BMEM(late_part) static volatile uint32_t late_data;

static struct k_thread changer;
static K_THREAD_STACK_DEFINE(changer_stack, 1024);

static void late_worker(void *arg)
{
	ARG_UNUSED(arg);
	worker_spinning = 1U;
	while (partition_added == 0U) {
	}
	late_data++;
	access_done = 1U;
}

static void late_app(void *arg)
{
	int h = app_svc_thread_spawn(late_worker, NULL, 0);

	ARG_UNUSED(arg);
	(void)app_svc_thread_join(h, -1);
	for (;;) {
		k_msleep(10);
	}
}

static bool has_partition(struct k_mem_domain *dom, struct k_mem_partition *part)
{
	for (int i = 0; i < CONFIG_MAX_DOMAIN_PARTITIONS; i++) {
		if (dom->partitions[i].start == part->start &&
		    dom->partitions[i].size == part->size) {
			return true;
		}
	}
	return false;
}

static void add_late_part(void *p_dom, void *p2, void *p3)
{
	int64_t end = k_uptime_get() + 2000;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (worker_spinning == 0U) {
		if (k_uptime_get() > end) {
			return;
		}
	}
	(void)k_mem_domain_add_partition(p_dom, &late_part);
	partition_added = 1U;
}

ZTEST(smp, test_domain_change_reaches_other_cpu)
{
	int app_cpu = arch_num_cpus() - 1;
	struct k_mem_domain *dom;
	struct app_platform_status st;
	int id;

	worker_spinning = 0U;
	partition_added = 0U;
	access_done = 0U;
	late_data = 0U;

	id = launch_on("t_late", late_app, BIT(app_cpu));
	zassert_true(id >= 0, "launch rc %d", id);
	dom = app_platform_domain(id);
	if (has_partition(dom, &late_part)) {
		zassert_equal(k_mem_domain_remove_partition(dom, &late_part), 0);
	}

	k_thread_create(&changer, changer_stack, K_THREAD_STACK_SIZEOF(changer_stack),
			add_late_part, dom, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_FOREVER);
	zassert_equal(k_thread_cpu_pin(&changer, (app_cpu + 1) % arch_num_cpus()), 0);
	k_thread_start(&changer);
	zassert_equal(k_thread_join(&changer, K_FOREVER), 0);
	zassert_equal(worker_spinning, 1U, "worker never ran");

	for (int i = 0; i < 100 && access_done == 0U; i++) {
		k_msleep(5);
	}
	zassert_equal(app_platform_status(id, &st), 0);
	zassert_false(st.faulted, "worker faulted on the partition added from another CPU");
	zassert_equal(access_done, 1U, "worker did not reach the partition");
	zassert_equal(late_data, 1U);

	zassert_equal(k_mem_domain_remove_partition(dom, &late_part), 0);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("t_late", 3000));
}
#endif

ZTEST_SUITE(smp, NULL, NULL, NULL, NULL, NULL);
