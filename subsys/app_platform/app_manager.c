/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Application group lifecycle: launch an application as an isolated user-mode
 * thread in its own memory domain, supervise it with a liveness watchdog, and
 * reclaim it on fault or hang without affecting the system or the other groups.
 */

#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/llext/llext.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

#define MAX_APPS    CONFIG_APP_PLATFORM_MAX_APPS
#define APP_STACK   CONFIG_APP_PLATFORM_APP_STACK_SIZE
#define APP_PRIO    CONFIG_APP_PLATFORM_APP_PRIORITY
#define WDT_TIMEOUT CONFIG_APP_PLATFORM_WDT_TIMEOUT_MS

/* A running app: an isolated user-mode group with its own domain + watchdog. */
struct running_app {
	const char         *name;
	bool                active;
	struct k_thread     thread;
	struct k_mem_domain domain;
	int                 wdt_chan;
	bool                wdt_fired;
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
		(void)k_msgq_put(&reclaim_q, &id, K_NO_WAIT);
	} else {
		k_fatal_halt(reason);
	}
}

static void wdt_cb(int chan, void *user)
{
	ARG_UNUSED(chan);
	struct running_app *a = user;
	uint8_t id = a - apps;

	a->wdt_fired = true;
	(void)k_msgq_put(&reclaim_q, &id, K_NO_WAIT);
}

void app_platform_heartbeat(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	if (a && a->wdt_chan >= 0) {
		(void)task_wdt_feed(a->wdt_chan);
	}
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
	k_thread_abort(&a->thread);
	a->active = false;
	LOG_INF("app '%s' reclaimed; system + other apps alive", a->name);
}

static void supervisor(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		uint8_t id;

		if (k_msgq_get(&reclaim_q, &id, K_FOREVER) == 0 && id < MAX_APPS) {
			reclaim(&apps[id]);
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
		  struct llext *ext)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (apps[i].active) {
			continue;
		}
		struct running_app *a = &apps[i];

		a->name = name;
		a->entry = entry;
		a->caps = caps;
		a->wdt_fired = false;
		k_mem_domain_init(&a->domain, 0, NULL);

		if (ext != NULL) {
#ifdef Z_LIBC_PARTITION_EXISTS
			k_mem_domain_add_partition(&a->domain, &z_libc_partition);
#endif
			if (llext_add_domain(ext, &a->domain) != 0) {
				LOG_ERR("could not map '%s' into its domain", name);
				return -ENOSPC;
			}
		}

		k_tid_t t = k_thread_create(&a->thread, app_stacks[i], APP_STACK,
					    trampoline, (void *)entry, NULL, NULL,
					    APP_PRIO, K_USER | K_INHERIT_PERMS,
					    K_FOREVER);
		k_thread_name_set(t, name);
		k_mem_domain_add_thread(&a->domain, t);
		a->wdt_chan = task_wdt_add(WDT_TIMEOUT, wdt_cb, a);
		a->active = true;
		k_thread_start(t);
		LOG_INF("launched app '%s' (isolated user-mode group %d)", name, i);
		return i;
	}
	return -ENOSPC;
}

int app_platform_launch(const struct app_def *def)
{
	return launch(def->name, def->entry, def->caps, NULL);
}

int app_platform_launch_llext(const char *name, struct llext *ext,
			      app_entry_t entry, uint32_t caps)
{
	return launch(name, entry, caps, ext);
}
