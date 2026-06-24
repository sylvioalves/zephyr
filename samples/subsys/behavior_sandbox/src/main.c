/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_surface.h"
#include "policy.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/fatal.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/app_memory/app_memdomain.h>

LOG_MODULE_REGISTER(behavior_sandbox, CONFIG_LOG_DEFAULT_LEVEL);

extern int cap_surface_init(void);

/*
 * Contain faults to the offending thread instead of halting the system. When
 * this handler returns, the kernel aborts the faulting thread and continues,
 * so a misbehaving sandbox cannot take down the supervisor or other sandboxes.
 * Kernel panics are not recoverable and must still halt.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_KERNEL_PANIC) {
		printk("supervisor: kernel panic, halting\n");
		k_fatal_halt(reason);
	}

	printk("supervisor: contained fault (reason %u) in thread %p, killing it\n",
	       reason, k_current_get());
}

#define SANDBOX_STACK_SIZE 2048
#define SANDBOX_PRIO       5
#define WDT_TIMEOUT_MS     1500

K_THREAD_STACK_DEFINE(sandbox_stack, SANDBOX_STACK_SIZE);
static struct k_thread sandbox_thread;

/* Private heap the sandbox is routed to; a future quota primitive caps this. */
K_HEAP_DEFINE(sandbox_heap, 1024);

/* Memory domain that isolates the sandbox from the rest of the system. */
static struct k_mem_domain sandbox_domain;

static volatile bool wdt_fired;
static int wdt_chan;

/* Signaled by the watchdog ISR callback; the supervisor thread does the abort. */
static K_SEM_DEFINE(wdt_kill_sem, 0, 1);

static void wdt_callback(int channel_id, void *user_data)
{
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	/*
	 * Runs in the system-clock ISR context. k_thread_abort() of another
	 * thread is not safe here, so only signal the supervisor thread, which
	 * performs the abort in thread context.
	 */
	wdt_fired = true;
	k_sem_give(&wdt_kill_sem);
}

static void wait_for_wdt_kill(k_tid_t tid)
{
	/*
	 * The watchdog tracks the sandbox's liveness. A runaway sandbox never
	 * yields a liveness signal, so the supervisor does not feed the channel.
	 * When the timeout fires, the ISR callback signals us and we abort the
	 * thread here, in thread context, then join the now-dead thread.
	 */
	k_sem_take(&wdt_kill_sem, K_FOREVER);
	printk("supervisor: watchdog fired, aborting runaway sandbox\n");
	k_thread_abort(tid);
	k_thread_join(tid, K_FOREVER);
}

static void run_demo(const char *name, enum policy_kind kind, bool guarded)
{
	printk("\n=== demo: %s ===\n", name);
	wdt_fired = false;

	k_tid_t tid = k_thread_create(&sandbox_thread, sandbox_stack,
				      SANDBOX_STACK_SIZE, policy_run,
				      (void *)(uintptr_t)kind, NULL, NULL,
				      SANDBOX_PRIO, K_USER | K_INHERIT_PERMS,
				      K_FOREVER);

	k_thread_heap_assign(&sandbox_thread, &sandbox_heap);

	if (k_mem_domain_add_thread(&sandbox_domain, tid) != 0) {
		printk("supervisor: failed to place thread in mem domain\n");
	}

	if (guarded) {
		wdt_chan = task_wdt_add(WDT_TIMEOUT_MS, wdt_callback, &sandbox_thread);
	}

	k_thread_start(tid);

	if (guarded) {
		wait_for_wdt_kill(tid);
		task_wdt_delete(wdt_chan);
	} else {
		k_thread_join(tid, K_FOREVER);
	}

	printk("supervisor: sandbox for '%s' is gone, system still alive\n", name);
}

int main(void)
{
	printk("\nbehavior sandbox PoC on %s\n", CONFIG_BOARD);

	if (cap_surface_init() != 0) {
		printk("supervisor: capability surface init failed\n");
		return 0;
	}

	if (task_wdt_init(NULL) != 0) {
		printk("supervisor: task_wdt_init failed\n");
		return 0;
	}

	k_mem_domain_init(&sandbox_domain, 0, NULL);

	run_demo("happy path (capabilities only)", POLICY_GOOD, false);
	run_demo("containment (raw access traps)", POLICY_ROGUE, false);
	run_demo("supervision (runaway killed)", POLICY_RUNAWAY, true);

	printk("\nall demos complete: supervisor survived every sandbox\n");
	return 0;
}
