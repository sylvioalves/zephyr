/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_rt.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>

#define APP_STACK_SIZE 2048
#define APP_PRIO       5

/* One app group: a domain plus a pool of thread slots that share it. */
static struct k_mem_domain domain;
static struct k_thread threads[APP_MAX_THREADS];
static K_THREAD_STACK_ARRAY_DEFINE(stacks, APP_MAX_THREADS, APP_STACK_SIZE);
static int thread_count;
static k_tid_t root_tid;

/*
 * Trampoline: runs in USER mode, so it must not read any kernel global. The
 * entry fn and arg are passed as thread parameters by value; the entry pointer
 * is app text the user thread may call, the arg is opaque.
 */
static void trampoline(void *entry, void *arg, void *u2)
{
	ARG_UNUSED(u2);
	app_entry_t fn = (app_entry_t)entry;

	fn(arg);
}

/* Spawn a thread into THIS app group (supervisor context). */
static int spawn_in_group(app_entry_t entry, void *arg, bool is_root)
{
	if (thread_count >= APP_MAX_THREADS) {
		return -ENOSPC;
	}
	int idx = thread_count++;

	uint32_t opts = K_USER | (is_root ? 0 : K_INHERIT_PERMS);

	k_tid_t t = k_thread_create(&threads[idx], stacks[idx], APP_STACK_SIZE,
				    trampoline, (void *)entry, arg, NULL,
				    APP_PRIO, opts, K_FOREVER);

	/* All threads of the app share its domain: same isolation, same memory,
	 * so they see each other's data but no other app's. Capability grants
	 * are applied per the app's manifest by the broker (omitted here; this
	 * sample focuses on the multi-thread, shared-domain model).
	 */
	k_mem_domain_add_thread(&domain, t);
	if (is_root) {
		root_tid = t;
	}
	k_thread_start(t);
	return idx;
}

int app_create(const char *name, struct k_mem_partition *part,
	       app_entry_t root, void *arg)
{
	printk("app-rt: creating app '%s' (group = domain + threads)\n", name);
	k_mem_domain_init(&domain, 0, NULL);
	if (part != NULL) {
		k_mem_domain_add_partition(&domain, part);
	}
	thread_count = 0;
	return spawn_in_group(root, arg, true);
}

void app_abort_all(void)
{
	for (int i = 0; i < thread_count; i++) {
		k_thread_abort(&threads[i]);
	}
	printk("app-rt: app aborted - all %d threads reaped together\n",
	       thread_count);
	thread_count = 0;
}

int z_impl_app_thread_spawn(app_entry_t entry, void *arg)
{
	/* A running app thread asks for another thread in its own group. The
	 * broker does the create in supervisor context, in the app's domain.
	 */
	return spawn_in_group(entry, arg, false);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_thread_spawn(app_entry_t entry, void *arg)
{
	/* entry is app text; arg is opaque to the kernel. The thread runs in the
	 * caller's domain with the caller's permissions, so no escalation.
	 */
	return z_impl_app_thread_spawn(entry, arg);
}
#include <zephyr/syscalls/app_thread_spawn_mrsh.c>
#endif
