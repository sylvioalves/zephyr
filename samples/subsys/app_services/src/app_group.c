/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_group.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fatal.h>
#include <zephyr/task_wdt/task_wdt.h>
#include "svc_registry.h"

#define SUPERVISOR_STACK_SIZE 2048
#define SUPERVISOR_PRIO       2
#define APP_PRIO              5

static struct app_group groups[APP_GROUP_MAX];
static K_THREAD_STACK_ARRAY_DEFINE(group_stacks, APP_GROUP_MAX, APP_GROUP_STACK_SIZE);

/*
 * Per-app resource pool: the kernel allocates from here on behalf of the app's
 * syscalls (sockets, DNS). Bounding it per app means one app cannot exhaust
 * kernel memory and starve another. This is the brokered-allocation model
 * (apps do not run a user-mode allocator; see V2 finding).
 */
#define APP_POOL_BYTES 4096
K_HEAP_DEFINE(app_pool_0, APP_POOL_BYTES);
K_HEAP_DEFINE(app_pool_1, APP_POOL_BYTES);
K_HEAP_DEFINE(app_pool_2, APP_POOL_BYTES);
K_HEAP_DEFINE(app_pool_3, APP_POOL_BYTES);
static struct k_heap *const app_pools[] = {
	&app_pool_0, &app_pool_1, &app_pool_2, &app_pool_3,
};
BUILD_ASSERT(ARRAY_SIZE(app_pools) >= APP_GROUP_MAX,
	     "need one resource pool per app group");

/* Reclaim requests from the wdt ISR / fatal handler to the supervisor thread. */
static struct k_msgq reclaim_q;
static uint8_t reclaim_q_buf[APP_GROUP_MAX * sizeof(uint8_t)];

/* The supervisor performs all aborts in thread context. */
static struct k_thread supervisor_thread;
static K_THREAD_STACK_DEFINE(supervisor_stack, SUPERVISOR_STACK_SIZE);

/* Map a thread back to its group (the manager owns every app thread). */
static struct app_group *group_of_thread(k_tid_t tid)
{
	for (int i = 0; i < APP_GROUP_MAX; i++) {
		if (groups[i].state != APP_GROUP_FREE &&
		    &groups[i].thread == tid) {
			return &groups[i];
		}
	}
	return NULL;
}

struct app_group *app_group_current(void)
{
	return group_of_thread(k_current_get());
}

/*
 * Fault containment. Runs in kernel context when an app faults. Returning
 * makes the kernel abort only the faulting thread; we also queue the group for
 * full reclaim by the supervisor. Kernel panics are not recoverable.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_KERNEL_PANIC) {
		printk("supervisor: kernel panic, halting\n");
		k_fatal_halt(reason);
	}

	struct app_group *g = group_of_thread(k_current_get());

	if (g != NULL) {
		printk("supervisor: app '%s' (group %u) faulted (reason %u); reclaiming\n",
		       g->name, g->id, reason);
		(void)k_msgq_put(&reclaim_q, &g->id, K_NO_WAIT);
	} else {
		printk("supervisor: fault in non-app thread %p, halting\n",
		       k_current_get());
		k_fatal_halt(reason);
	}
}

/* Per-group watchdog timeout: queue the group for reclaim (ISR context). */
static void wdt_callback(int channel_id, void *user_data)
{
	ARG_UNUSED(channel_id);
	struct app_group *g = user_data;

	(void)k_msgq_put(&reclaim_q, &g->id, K_NO_WAIT);
}

void app_group_abort(struct app_group *g)
{
	if (g->state == APP_GROUP_FREE) {
		return;
	}

	/* 1. stop watching this group's liveness. */
	if (g->wdt_chan >= 0) {
		(void)task_wdt_delete(g->wdt_chan);
		g->wdt_chan = -1;
	}

	/* 2. kill the app thread (synchronous on this arch for these threads). */
	k_thread_abort(&g->thread);

	/* 3. revoke every service grant - no dangling capabilities. */
	svc_registry_revoke_all(g);
	g->granted_services = 0;

	/*
	 * 4. reclaim the domain by REMOVING the app's partition (RISC-V has no
	 * k_mem_domain_deinit). The domain object is kept and re-provisioned on
	 * the next create that reuses this slot.
	 */
	if (g->part != NULL) {
		(void)k_mem_domain_remove_partition(&g->domain, g->part);
		g->part = NULL;
	}

	g->state = APP_GROUP_FREE;
	printk("supervisor: group %u ('%s') reclaimed; slot free\n", g->id, g->name);
}

static void supervisor_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		uint8_t id;

		if (k_msgq_get(&reclaim_q, &id, K_FOREVER) == 0 &&
		    id < APP_GROUP_MAX) {
			app_group_abort(&groups[id]);
		}
	}
}

int app_group_init(void)
{
	for (int i = 0; i < APP_GROUP_MAX; i++) {
		groups[i].id = i;
		groups[i].state = APP_GROUP_FREE;
		groups[i].stack = group_stacks[i];
		groups[i].wdt_chan = -1;
	}

	k_msgq_init(&reclaim_q, reclaim_q_buf, sizeof(uint8_t), APP_GROUP_MAX);

	if (task_wdt_init(NULL) != 0) {
		printk("supervisor: task_wdt_init failed\n");
		return -EIO;
	}

	k_thread_create(&supervisor_thread, supervisor_stack, SUPERVISOR_STACK_SIZE,
			supervisor_fn, NULL, NULL, NULL,
			SUPERVISOR_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&supervisor_thread, "supervisor");
	return 0;
}

struct app_group *app_group_create(const char *name,
				   struct k_mem_partition *part,
				   app_entry_t entry, void *arg)
{
	for (int i = 0; i < APP_GROUP_MAX; i++) {
		struct app_group *g = &groups[i];

		if (g->state != APP_GROUP_FREE) {
			continue;
		}

		g->name = name;
		g->part = part;
		g->entry = entry;
		g->arg = arg;
		g->granted_services = 0;
		g->wdt_chan = -1;
		k_sem_init(&g->heartbeat, 0, 1);

		/* (Re)provision the reused domain with this app's partition. */
		k_mem_domain_init(&g->domain, 0, NULL);
		if (part != NULL) {
			k_mem_domain_add_partition(&g->domain, part);
		}

		g->state = APP_GROUP_READY;
		return g;
	}

	return NULL;
}

int app_group_grant_service(struct app_group *g, uint32_t service_id)
{
	if (g->state != APP_GROUP_READY) {
		return -EINVAL;
	}
	return svc_registry_grant(g, service_id);
}

/*
 * Trampoline: runs in USER MODE, so it must not dereference the kernel-side
 * group struct. The entry fn, the group ptr, and arg are passed as thread
 * parameters by value; the entry fn treats the group ptr as an opaque token
 * (it only ever passes it back through broker syscalls, never dereferences it).
 */
static void app_trampoline(void *entry, void *gp, void *arg)
{
	app_entry_t fn = (app_entry_t)entry;

	fn((struct app_group *)gp, arg);
}

int app_group_start(struct app_group *g)
{
	if (g->state != APP_GROUP_READY) {
		return -EINVAL;
	}

	k_tid_t t = k_thread_create(&g->thread, g->stack, APP_GROUP_STACK_SIZE,
				    app_trampoline, (void *)g->entry, g, g->arg,
				    APP_PRIO, K_USER | K_INHERIT_PERMS, K_FOREVER);
	k_thread_name_set(t, g->name);

	/* Bound the app's kernel-side allocations to its own resource pool. */
	k_thread_heap_assign(t, app_pools[g->id]);

	/* The app must reach its liveness sem; grant it. */
	k_thread_access_grant(t, &g->heartbeat);

	/* Apply the service-token grants recorded by app_group_grant_service(). */
	for (uint32_t id = 0; id < SVC_COUNT; id++) {
		if ((g->granted_services & BIT(id)) == 0) {
			continue;
		}
		const struct svc_desc *svc = svc_registry_get(id);

		if (svc != NULL && svc->token != NULL) {
			k_object_access_grant(svc->token, t);
		}
	}

	if (g->part != NULL) {
		k_mem_domain_add_thread(&g->domain, t);
	}

	/* Arm this group's liveness watchdog. */
	g->wdt_chan = task_wdt_add(APP_GROUP_HEARTBEAT_MS, wdt_callback, g);

	g->state = APP_GROUP_RUNNING;
	k_thread_start(t);
	return 0;
}

void app_group_heartbeat(struct app_group *g)
{
	if (g->wdt_chan >= 0) {
		(void)task_wdt_feed(g->wdt_chan);
	}
}
