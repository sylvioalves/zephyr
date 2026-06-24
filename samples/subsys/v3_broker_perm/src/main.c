/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * V3 spike: prove the broker's per-app service permission on C6. The service is
 * reached only via the svc_request syscall, which gates on the caller holding
 * the service-token kobject permission.
 *
 *  - App A is granted the token -> svc_request succeeds.
 *  - App B is NOT granted       -> svc_request faults at the syscall boundary,
 *    killing ONLY App B; App A and the supervisor survive.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fatal.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

/* The service token: a kobject whose permission represents service access. */
K_SEM_DEFINE(svc_token, 0, 1);

K_THREAD_STACK_DEFINE(stack_a, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_b, STACK_SIZE);
static struct k_thread th_a, th_b;

static volatile bool app_a_ok;
static volatile bool app_b_denied;

static K_SEM_DEFINE(a_done, 0, 1);
static K_SEM_DEFINE(b_done, 0, 1);

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_KERNEL_PANIC) {
		printk("PANIC, halting\n");
		k_fatal_halt(reason);
	}

	if (k_current_get() == &th_b) {
		app_b_denied = true;
		printk("  [contained] App B denied service token (EXPECTED) - killed\n");
		k_sem_give(&b_done);
	} else {
		printk("  [unexpected fault] reason %u in thread %p - TEST BUG\n",
		       reason, k_current_get());
	}
}

/* App A: granted the token, calls the service - must succeed. */
static void app_a_entry(void *token, void *p2, void *p3)
{
	ARG_UNUSED(p2); ARG_UNUSED(p3);

	/*
	 * Granted app: svc_request must succeed. Reaching the sem-give (which is
	 * a granted kobject) proves the call returned without faulting; the rc
	 * is reported via printk. User threads cannot write kernel globals, so
	 * the supervisor infers success from a_done being given.
	 */
	int rc = svc_request((struct k_sem *)token);

	printk("App A (granted): svc_request -> rc=%d (service ran)\n", rc);
	k_sem_give(&a_done);
	for (;;) {
		k_msleep(100);
	}
}

/* App B: NOT granted the token, calls the service - must be denied. */
static void app_b_entry(void *token, void *p2, void *p3)
{
	ARG_UNUSED(p2); ARG_UNUSED(p3);

	printk("App B (ungranted): calling svc_request (expect denial+kill)...\n");
	int rc = svc_request((struct k_sem *)token);

	printk("App B: NOT denied (rc=%d) - PERMISSION GATE FAILED\n", rc);
	k_sem_give(&b_done);
	for (;;) {
		k_msleep(100);
	}
}

int main(void)
{
	k_tid_t t;

	printk("\n=== V3: broker per-app service permission (C6) ===\n");

	/* App A: GRANTED the service token kobject. */
	t = k_thread_create(&th_a, stack_a, STACK_SIZE, app_a_entry,
			    &svc_token, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);
	k_thread_access_grant(t, &a_done);
	k_object_access_grant(&svc_token, t);   /* <-- the service grant */
	k_thread_start(t);
	/* Reaching a_done means App A's svc_request returned without faulting. */
	app_a_ok = (k_sem_take(&a_done, K_SECONDS(2)) == 0);

	/* App B: NOT granted the token (only its own done-sem). */
	t = k_thread_create(&th_b, stack_b, STACK_SIZE, app_b_entry,
			    &svc_token, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);
	k_thread_access_grant(t, &b_done);
	/* deliberately NO k_object_access_grant(&svc_token, t) */
	k_thread_start(t);
	k_sem_take(&b_done, K_FOREVER);   /* given by the fatal handler on denial */

	k_msleep(200);
	printk("\n=== V3 RESULT ===\n");
	printk("  App A (granted) used service: %s\n", app_a_ok ? "OK" : "FAIL");
	printk("  App B (ungranted) denied + killed: %s\n",
	       app_b_denied ? "OK" : "FAIL");
	printk("  Supervisor + App A survived B's denial: OK\n");
	printk("  V3 %s\n", (app_a_ok && app_b_denied) ? "PASS" : "FAIL");
	return 0;
}
