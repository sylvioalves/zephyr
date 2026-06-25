/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A multi-threaded app. The app is one group (domain + threads). Its root
 * thread spawns two more threads in the same group; all three share the app's
 * memory and isolation, and are reaped together when the app is aborted - the
 * process-with-threads model on a microcontroller.
 */

#include "app_rt.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/app_memory/app_memdomain.h>

/* Shared app state, in the app's own partition - all the app's threads see it,
 * no other app does.
 */
K_APPMEM_PARTITION_DEFINE(app_part);
static volatile int K_APP_DMEM(app_part) shared_counter;

/* A user-mode delay (a real SDK exposes app_sleep_ms; busy-wait keeps this
 * sample free of any syscall the demo app is not granted).
 */
static void spin_delay(void)
{
	for (volatile int i = 0; i < 4000000; i++) {
	}
}

/* A worker thread: bumps the shared counter (proving shared app memory). */
static void worker(void *name_p)
{
	const char *name = name_p;

	for (int i = 0; i < 6; i++) {
		shared_counter++;
		printk("    [%s] tick %d, shared_counter=%d (shared app memory)\n",
		       name, i, shared_counter);
		spin_delay();
	}
}

/* The app's root thread: spawns two more threads in its own group. */
static void app_root(void *arg)
{
	ARG_UNUSED(arg);

	printk("  [root] app started; spawning two worker threads in my group\n");
	app_thread_spawn(worker, "worker-A");
	app_thread_spawn(worker, "worker-B");

	/* Root also does work. */
	worker("root");
}

int main(void)
{
	printk("\n*** multi-threaded app: one app, many threads, shared domain (C6) ***\n");

	struct k_mem_partition *part = &app_part;

	app_create("multi", part, app_root, NULL);

	/* Let the app run, then tear the whole thing down. */
	k_msleep(4000);
	app_abort_all();

	printk("\n*** done: app ran 3 threads sharing its memory; aborted together ***\n");
	return 0;
}
