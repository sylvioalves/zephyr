/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* App runtime: spawn extra threads into the calling app's group. */

#include "app_sdk.h"
#include <zephyr/kernel.h>

#define RT_MAX_THREADS 3
#define RT_STACK_SIZE  2048
#define RT_PRIO        5

static struct k_thread rt_threads[RT_MAX_THREADS];
static K_THREAD_STACK_ARRAY_DEFINE(rt_stacks, RT_MAX_THREADS, RT_STACK_SIZE);
static int rt_count;
static struct k_mem_domain *rt_domain;

void app_rt_set_domain(struct k_mem_domain *dom)
{
	rt_domain = dom;
}

static void rt_trampoline(void *entry, void *arg, void *u2)
{
	ARG_UNUSED(u2);
	void (*fn)(void *) = (void (*)(void *))entry;

	fn(arg);
}

int app_rt_spawn(app_entry_t entry, void *arg)
{
	if (rt_count >= RT_MAX_THREADS) {
		return -ENOSPC;
	}
	int idx = rt_count++;

	k_tid_t t = k_thread_create(&rt_threads[idx], rt_stacks[idx],
				    RT_STACK_SIZE, rt_trampoline,
				    (void *)entry, arg, NULL,
				    RT_PRIO, K_USER | K_INHERIT_PERMS, K_FOREVER);
	if (rt_domain) {
		k_mem_domain_add_thread(rt_domain, t);
	}
	k_thread_start(t);
	return idx;
}

int app_rt_join(int idx)
{
	if (idx < 0 || idx >= rt_count) {
		return -EINVAL;
	}

	return k_thread_join(&rt_threads[idx], K_FOREVER);
}
