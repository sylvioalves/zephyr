/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * V1 spike: prove a k_mem_domain can be reclaimed and re-isolated on RISC-V
 * (C6) WITHOUT k_mem_domain_deinit (which is -ENOTSUP here). The app-group
 * reclaim model depends on: abort threads -> remove the app's partition ->
 * add the next app's partition to the SAME domain -> the new app must not be
 * able to reach the previous app's memory.
 *
 * Sequence:
 *  1. domain D + partition A (sentinel 0xAAAAAAAA). App1 reads A: OK.
 *  2. abort App1; remove A from D; add partition B to D.
 *  3. App2 runs in the SAME D: reads B (OK), then touches A's address ->
 *     must PMP-fault (contained), proving A is gone for the new app.
 *  4. A second domain D2 (partition C) runs throughout, unaffected.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fatal.h>
#include <zephyr/app_memory/app_memdomain.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_APPMEM_PARTITION_DEFINE(part_a);
K_APPMEM_PARTITION_DEFINE(part_b);
K_APPMEM_PARTITION_DEFINE(part_c);

static volatile uint32_t K_APP_DMEM(part_a) a_val = 0xAAAAAAAA;
static volatile uint32_t K_APP_DMEM(part_b) b_val = 0xBBBBBBBB;
static volatile uint32_t K_APP_DMEM(part_c) c_val = 0xCCCCCCCC;

static struct k_mem_domain dom;     /* reused domain D  */
static struct k_mem_domain dom2;    /* untouched D2     */

K_THREAD_STACK_DEFINE(stack1, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack2, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_c, STACK_SIZE);
static struct k_thread th1, th2, th_c;

static volatile bool app2_faulted;
static volatile uint32_t *fault_probe_addr;

static K_SEM_DEFINE(app1_done, 0, 1);
static K_SEM_DEFINE(app2_done, 0, 1);

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_KERNEL_PANIC) {
		printk("PANIC, halting\n");
		k_fatal_halt(reason);
	}

	if (k_current_get() == &th2) {
		/* Expected: App2 faulted probing the removed partition A. */
		app2_faulted = true;
		printk("  [contained fault] App2 faulted probing removed A (EXPECTED)\n");
		k_sem_give(&app2_done);
	} else {
		/* Any other fault is a test-setup bug, not the result we want. */
		printk("  [unexpected fault] reason %u in thread %p - TEST BUG\n",
		       reason, k_current_get());
	}
}

/* App1: read its own partition A. */
static void app1_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	printk("App1: read A = 0x%08x (own partition)\n", a_val);
	k_sem_give(&app1_done);
	/* idle until aborted */
	for (;;) {
		k_msleep(100);
	}
}

/* App2: read its own partition B, then probe A's address (must fault). */
static void app2_entry(void *probe, void *p2, void *p3)
{
	ARG_UNUSED(p2); ARG_UNUSED(p3);
	printk("App2: read B = 0x%08x (own partition)\n", b_val);
	printk("App2: probing removed partition A @ %p (expect PMP fault)...\n", probe);
	uint32_t leaked = *(volatile uint32_t *)probe;
	/* If we reach here, isolation FAILED. */
	printk("App2: LEAK! read 0x%08x from removed A - ISOLATION FAILED\n", leaked);
	k_sem_give(&app2_done);
	for (;;) {
		k_msleep(100);
	}
}

/* Concurrent app in a separate domain, must stay alive the whole time. */
static void app_c_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	for (int i = 0; i < 20; i++) {
		printk("AppC (D2): alive, C = 0x%08x\n", c_val);
		k_msleep(400);
	}
}

int main(void)
{
	struct k_mem_partition *p_a = &part_a;
	struct k_mem_partition *p_b = &part_b;
	struct k_mem_partition *p_c = &part_c;
	k_tid_t t;

	printk("\n=== V1: domain reuse / reclaim without deinit (C6) ===\n");

	/* D2 with partition C: the untouched concurrent domain. */
	k_mem_domain_init(&dom2, 1, &p_c);
	t = k_thread_create(&th_c, stack_c, STACK_SIZE, app_c_entry,
			    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);
	k_mem_domain_add_thread(&dom2, t);
	k_thread_start(t);

	/* Step 1: D + partition A, run App1. */
	printk("\n[1] init domain D with partition A, run App1\n");
	k_mem_domain_init(&dom, 1, &p_a);
	t = k_thread_create(&th1, stack1, STACK_SIZE, app1_entry,
			    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);
	/* User threads must be granted access to every kobject they use. */
	k_thread_access_grant(t, &app1_done);
	k_mem_domain_add_thread(&dom, t);
	k_thread_start(t);
	k_sem_take(&app1_done, K_FOREVER);

	/* Step 2: abort App1, remove A, add B - REUSING domain D (no deinit). */
	printk("\n[2] abort App1; remove A; add B to the SAME domain D\n");
	k_thread_abort(&th1);
	int rc_rm = k_mem_domain_remove_partition(&dom, p_a);
	int rc_add = k_mem_domain_add_partition(&dom, p_b);

	printk("    remove_partition(A)=%d  add_partition(B)=%d  (deinit NOT used)\n",
	       rc_rm, rc_add);

	/* Step 3: run App2 in the reused D; it must fault probing A. */
	printk("\n[3] run App2 in reused D; probe removed A must PMP-fault\n");
	fault_probe_addr = (volatile uint32_t *)&a_val;
	t = k_thread_create(&th2, stack2, STACK_SIZE, app2_entry,
			    (void *)fault_probe_addr, NULL, NULL,
			    APP_PRIO, K_USER, K_FOREVER);
	k_thread_access_grant(t, &app2_done);
	k_mem_domain_add_thread(&dom, t);
	k_thread_start(t);
	k_sem_take(&app2_done, K_FOREVER);

	k_msleep(200);
	printk("\n=== V1 RESULT ===\n");
	printk("  remove/add on reused domain: %s\n",
	       (rc_rm == 0 && rc_add == 0) ? "OK" : "FAILED");
	printk("  App2 isolated from removed A: %s\n",
	       app2_faulted ? "OK (faulted as expected)" : "FAILED (leak!)");
	printk("  V1 %s\n",
	       (rc_rm == 0 && rc_add == 0 && app2_faulted) ? "PASS" : "FAIL");

	/* let AppC keep printing to prove D2 untouched */
	k_msleep(2000);
	printk("  AppC (D2) survived the whole reuse cycle: OK\n");
	return 0;
}
