/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Scale and robustness runs. MODE: 1 max apps + boot time, 2 load/unload
 * cycles, 3 fault storm, 4 watchdog under load.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/multi_heap/shared_multi_heap.h>
#include <zephyr/llext/llext.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_heap.h>
#include <string.h>

#ifndef MODE
#define MODE 1
#endif
#ifndef N_CYCLES
#define N_CYCLES 1000
#endif
#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME)
#define LLEXT_PSRAM_HEAP (512 * 1024)

static const uint8_t loop_app[] = {
#include "loop_app.inc"
};
static const uint8_t loop_hash[32] = {
#include "loop_app.sha256.inc"
};

extern struct k_heap _system_heap;
extern struct k_heap llext_heap;

static char names[CONFIG_APP_PLATFORM_MAX_APPS][12];

/* Total relaunches the supervisor has performed, across every slot. */
static uint32_t restart_total(void)
{
	uint32_t n = 0;

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0) {
			n += st.restarts;
		}
	}
	return n;
}


static void heap_line(const char *tag)
{
	struct sys_memory_stats s1, s2;

	sys_heap_runtime_stats_get(&_system_heap.heap, &s1);
	sys_heap_runtime_stats_get(&llext_heap.heap, &s2);
	printk("  %-26s sys heap alloc %6zu (max %6zu)  llext heap alloc %7zu (max %7zu)\n",
	       tag, s1.allocated_bytes, s1.max_allocated_bytes, s2.allocated_bytes,
	       s2.max_allocated_bytes);
}

static int load_launch(const char *name)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load(name, loop_app, ARRAY_SIZE(loop_app),
					loop_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		return rc;
	}
	rc = app_platform_launch_llext(name, ext, (app_entry_t)entry, NULL, APP_CAPS);
	if (rc < 0) {
		app_loader_forget(ext);
		llext_unload(&ext);
	}
	return rc;
}

static bool wait_slot_free(int id, int ms)
{
	for (int i = 0; i < ms; i++) {
		struct app_platform_status st;

		if (app_platform_status(id, &st) == -EAGAIN) {
			return true;
		}
		k_msleep(1);
	}
	return false;
}

static int psram_heap(void)
{
	void *heap = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 64,
						     LLEXT_PSRAM_HEAP);

	if (heap == NULL || llext_heap_init(heap, LLEXT_PSRAM_HEAP) != 0) {
		printk("llext heap in PSRAM failed\n");
		return -ENOMEM;
	}
	printk("llext heap: %u KB in PSRAM at %p\n", LLEXT_PSRAM_HEAP / 1024, heap);
	return 0;
}

#if MODE == 1
static void mode_max(void)
{
	int64_t t0 = k_uptime_get();
	int launched = 0;

	printk("== 5.1/3.5 max concurrent llext apps (MAX_APPS=%d), uptime at start %lld ms\n",
	       CONFIG_APP_PLATFORM_MAX_APPS, t0);
	app_platform_log_enable(false);
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		snprintk(names[i], sizeof(names[i]), "app%02d", i);
		int rc = load_launch(names[i]);

		if (rc < 0) {
			printk("  launch #%d FAILED: %d\n", i, rc);
			heap_line("at failure");
			break;
		}
		launched++;
		if (launched == 1 || launched == 4 || launched == 16 ||
		    launched == 37) {
			printk("  %2d apps running at uptime %lld ms (+%lld ms since start)\n",
			       launched, k_uptime_get(), k_uptime_get() - t0);
		}
	}
	printk("  launched %d apps, %lld ms since start\n", launched,
	       k_uptime_get() - t0);
	heap_line("all launched");
	k_msleep(5000);
	int alive = 0;

	for (int i = 0; i < launched; i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0 && st.active) {
			alive++;
		}
	}
	printk("  %d / %d apps alive after 5 s\n", alive, launched);
}
#endif

#if MODE == 2
static void mode_cycles(void)
{
	printk("== 5.2 %d load/launch/remove cycles of one llext app\n", N_CYCLES);
	app_platform_log_enable(false);
	heap_line("before");
	int64_t t0 = k_uptime_get();
	int fails = 0;

	for (int i = 0; i < N_CYCLES; i++) {
		int id = load_launch("cyc");

		if (id < 0) {
			fails++;
			printk("  cycle %d: launch failed %d\n", i, id);
			continue;
		}
		k_msleep(2);
		app_platform_remove(id);
		if (!wait_slot_free(id, 500)) {
			fails++;
			printk("  cycle %d: slot never freed\n", i);
		}
		if ((i % 200) == 199) {
			heap_line("progress");
		}
	}
	printk("  done: %d cycles, %d failures, %lld ms total (%lld ms/cycle)\n",
	       N_CYCLES, fails, k_uptime_get() - t0, (k_uptime_get() - t0) / N_CYCLES);
	heap_line("after");
	printk("  slots in use: %d\n", app_platform_count());
}
#endif

#if MODE == 3
static void storm_app(void *arg)
{
	ARG_UNUSED(arg);
	volatile uint32_t *p = NULL;
	*p = 1;
}

static void mode_storm(void)
{
	const struct app_def def = {
		.name = "storm", .source = APP_BUILTIN, .entry = storm_app,
		.caps = APP_CAP_LOG,
	};

	printk("== 5.3 fault storm: app crashes at launch, restart policy on, until %d restarts\n",
	       N_CYCLES);
	app_platform_log_enable(false);
	heap_line("before");
	int64_t t0 = k_uptime_get();
	int id = app_platform_launch(&def);

	app_platform_set_restart(id, true);
	uint32_t last = 0;

	int64_t tp = t0;

	while (restart_total() < (uint32_t)N_CYCLES) {
		k_msleep(100);
		if (restart_total() - last >= 200 || k_uptime_get() - tp > 2000) {
			last = restart_total();
			tp = k_uptime_get();
			printk("  %u restarts, main alive at %lld ms, slots:", last, tp - t0);
			for (int i = 0; i < 4; i++) {
				struct app_platform_status st;
				int rc = app_platform_status(i, &st);

				printk(" [%d rc%d a%d f%d]", i, rc, rc == 0 ? st.active : -1,
				       rc == 0 ? st.faulted : -1);
			}
			printk("\n");
		}
		if (k_uptime_get() - t0 > 600000) {
			printk("  timeout\n");
			break;
		}
	}
	uint32_t total = restart_total();

	printk("  %u restarts in %lld ms (%lld ms each)\n", total,
	       k_uptime_get() - t0,
	       (total > 0U) ? (k_uptime_get() - t0) / total : 0);
	/* Stop the storm and prove the platform still works. */
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_set_restart(i, false);
	}
	k_msleep(200);
	heap_line("after storm");
	int nid = load_launch("after");

	printk("  llext app launch after storm: %s (id %d)\n", nid >= 0 ? "ok" : "FAILED", nid);
}
#endif

#if MODE == 4
static void busy_app(void *arg)
{
	ARG_UNUSED(arg);
	volatile uint32_t x = 0;

	for (;;) {
		x++;
	}
}

static void beat_app(void *arg)
{
	ARG_UNUSED(arg);
	for (;;) {
		svc_heartbeat();
		k_msleep(100);
	}
}

static void mode_wdt(void)
{
	printk("== 5.5 watchdog under load: 36 busy apps + 1 monitored heartbeat app, 60 s\n");
	app_platform_log_enable(false);
	const struct app_def beat = {
		.name = "beat", .source = APP_BUILTIN, .entry = beat_app,
		.caps = APP_CAP_LOG, .monitored = true,
	};
	int bid = app_platform_launch(&beat);

	for (int i = 0; i < 36; i++) {
		snprintk(names[i], sizeof(names[i]), "busy%02d", i);
		const struct app_def b = {
			.name = names[i], .source = APP_BUILTIN, .entry = busy_app,
			.caps = APP_CAP_LOG,
		};
		if (app_platform_launch(&b) < 0) {
			printk("  busy launch %d failed\n", i);
		}
	}
	printk("  %d apps running\n", app_platform_count());
	for (int s = 0; s < 60; s += 10) {
		k_msleep(10000);
		struct app_platform_status st;
		int rc = app_platform_status(bid, &st);

		printk("  t=%2d s: heartbeat app %s, faulted=%d, %d apps\n", s + 10,
		       (rc == 0 && st.active) ? "alive" : "RECLAIMED", rc == 0 ? st.faulted : -1,
		       app_platform_count());
	}
}
#endif

int main(void)
{
	printk("\n*** app_scale MODE=%d (C5) ***\n", MODE);
	if (psram_heap() != 0 || app_platform_init() != 0) {
		return 0;
	}
#if MODE == 1
	mode_max();
#elif MODE == 2
	mode_cycles();
#elif MODE == 3
	mode_storm();
#elif MODE == 4
	mode_wdt();
#endif
	printk("*** app_scale done ***\n");
	return 0;
}
