/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "platform.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fatal.h>
#include <zephyr/task_wdt/task_wdt.h>

#define APP_STACK_SIZE 2048
#define APP_PRIO       5

/* A running app: an isolated user-mode group with its own domain + watchdog. */
struct running_app {
	const char         *name;
	bool                active;
	struct k_thread     thread;
	struct k_mem_domain domain;
	int                 wdt_chan;
	bool                wdt_fired;
	app_entry_t         entry;
};

static struct running_app apps[PLATFORM_MAX_APPS];
static K_THREAD_STACK_ARRAY_DEFINE(app_stacks, PLATFORM_MAX_APPS, APP_STACK_SIZE);

/* Reclaim requests from the wdt ISR / fatal handler -> supervisor thread. */
static struct k_msgq reclaim_q;
static uint8_t reclaim_buf[PLATFORM_MAX_APPS];
static struct k_thread sup_thread;
static K_THREAD_STACK_DEFINE(sup_stack, 2048);

static struct running_app *app_of_thread(k_tid_t t)
{
	for (int i = 0; i < PLATFORM_MAX_APPS; i++) {
		if (apps[i].active && &apps[i].thread == t) {
			return &apps[i];
		}
	}
	return NULL;
}

int manager_id_of_current(void)
{
	struct running_app *a = app_of_thread(k_current_get());

	return a ? (int)(a - apps) : -1;
}

/* Fault containment: a faulting app is reclaimed, the system survives. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	if (reason == K_ERR_KERNEL_PANIC) {
		printk("platform: kernel panic, halting\n");
		k_fatal_halt(reason);
	}
	struct running_app *a = app_of_thread(k_current_get());

	if (a) {
		uint8_t id = a - apps;

		printk("platform: app '%s' faulted (reason %u) - reclaiming\n",
		       a->name, reason);
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

void manager_app_feed(void)
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
	printk("platform: app '%s' reclaimed; system + other apps alive\n",
	       a->name);
}

static void supervisor(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		uint8_t id;

		if (k_msgq_get(&reclaim_q, &id, K_FOREVER) == 0 &&
		    id < PLATFORM_MAX_APPS) {
			reclaim(&apps[id]);
		}
	}
}

static void trampoline(void *entry, void *u1, void *u2)
{
	ARG_UNUSED(u1); ARG_UNUSED(u2);
	((app_entry_t)entry)(NULL);
}

int manager_init(void)
{
	k_msgq_init(&reclaim_q, reclaim_buf, sizeof(uint8_t), PLATFORM_MAX_APPS);
	if (task_wdt_init(NULL) != 0) {
		return -EIO;
	}
	for (int i = 0; i < PLATFORM_MAX_APPS; i++) {
		apps[i].wdt_chan = -1;
	}
	k_thread_create(&sup_thread, sup_stack, 2048, supervisor,
			NULL, NULL, NULL, 2, 0, K_NO_WAIT);
	k_thread_name_set(&sup_thread, "supervisor");
	return 0;
}

/* Launch an app from its catalog entry as an isolated, supervised group. */
int manager_launch(const struct app_def *def)
{
	for (int i = 0; i < PLATFORM_MAX_APPS; i++) {
		if (apps[i].active) {
			continue;
		}
		struct running_app *a = &apps[i];

		a->name = def->name;
		a->entry = def->entry;
		a->wdt_fired = false;
		k_mem_domain_init(&a->domain, 0, NULL);

		k_tid_t t = k_thread_create(&a->thread, app_stacks[i],
					    APP_STACK_SIZE, trampoline,
					    (void *)def->entry, NULL, NULL,
					    APP_PRIO, K_USER | K_INHERIT_PERMS,
					    K_FOREVER);
		k_thread_name_set(t, def->name);
		k_mem_domain_add_thread(&a->domain, t);
		a->wdt_chan = task_wdt_add(2000, wdt_cb, a);
		a->active = true;
		k_thread_start(t);
		printk("platform: launched app '%s' (isolated group %d)\n",
		       def->name, i);
		return i;
	}
	return -ENOSPC;
}
