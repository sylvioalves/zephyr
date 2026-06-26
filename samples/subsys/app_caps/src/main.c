/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Capability layer demo. Apps hold unforgeable capability tokens. This shows
 * the four properties that make it a capability system rather than an access
 * list: a holder may use its capability; a forged token is rejected; the holder
 * may delegate the capability to another app (which then gains it and the
 * sender loses it); and revocation kills every token for the resource.
 *
 * The two app threads step through the flow under supervisor coordination so
 * the cause and effect of each property is visible in order.
 */

#include "cap.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#if defined(CONFIG_USERSPACE)
#include <zephyr/app_memory/app_memdomain.h>
#endif

#define STACK_SIZE 2048
#define APP_PRIO   5

/*
 * Apps run in user mode where the target supports it (e.g. RISC-V PMP on C6),
 * proving the capability syscalls cross the privilege boundary safely. On
 * targets without userspace (e.g. native_sim), the same capability LOGIC -
 * unforgeable tokens, delegation, revocation - runs with plain threads; the
 * security mechanism is identical, only the isolation boundary differs.
 */
#if defined(CONFIG_USERSPACE)
#define APP_OPTS (K_USER)
K_APPMEM_PARTITION_DEFINE(cap_part);
static struct k_mem_domain cap_domain;
static cap_t K_APP_DMEM(cap_part) the_cap;
static k_tid_t K_APP_DMEM(cap_part) tid_b;
#else
#define APP_OPTS (0)
static cap_t the_cap;
static k_tid_t tid_b;
#endif

K_THREAD_STACK_DEFINE(stack_a, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_b, STACK_SIZE);
static struct k_thread th_a, th_b;

/* Step gates, given by the supervisor to sequence the demo. */
static K_SEM_DEFINE(go_a_use,      0, 1);
static K_SEM_DEFINE(done_a_use,    0, 1);
static K_SEM_DEFINE(go_a_delegate, 0, 1);
static K_SEM_DEFINE(done_a_deleg,  0, 1);
static K_SEM_DEFINE(go_b_use,      0, 1);
static K_SEM_DEFINE(done_b_use,    0, 1);
static K_SEM_DEFINE(go_b_again,    0, 1);
static K_SEM_DEFINE(done_b_again,  0, 1);

static void app_a(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	k_sem_take(&go_a_use, K_FOREVER);
	printk("\n[A] (1) use my capability:\n");
	cap_invoke(the_cap);
	printk("[A] (2) try to FORGE a token (made-up slot+nonce):\n");
	cap_invoke(0x0000000512345678ULL);
	k_sem_give(&done_a_use);

	k_sem_take(&go_a_delegate, K_FOREVER);
	printk("\n[A] (3) delegate my capability to app B:\n");
	cap_delegate(the_cap, tid_b);
	printk("[A] (4) try to use it AFTER delegating it away:\n");
	cap_invoke(the_cap);   /* A is no longer the holder -> denied */
	k_sem_give(&done_a_deleg);
}

static void app_b(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	k_sem_take(&go_b_use, K_FOREVER);
	printk("\n[B] (5) use the capability delegated to me:\n");
	cap_invoke(the_cap);
	k_sem_give(&done_b_use);

	k_sem_take(&go_b_again, K_FOREVER);
	printk("\n[B] (7) try to use it AFTER revocation:\n");
	cap_invoke(the_cap);   /* revoked -> denied */
	k_sem_give(&done_b_again);
}

int main(void)
{
	printk("\n*** capability layer: unforgeable, delegatable, revocable ***\n");

	k_tid_t ta = k_thread_create(&th_a, stack_a, STACK_SIZE, app_a,
				     NULL, NULL, NULL, APP_PRIO, APP_OPTS, K_FOREVER);
	k_tid_t tb = k_thread_create(&th_b, stack_b, STACK_SIZE, app_b,
				     NULL, NULL, NULL, APP_PRIO, APP_OPTS, K_FOREVER);
	tid_b = tb;

#if defined(CONFIG_USERSPACE)
	struct k_mem_partition *parts[] = { &cap_part };

	k_mem_domain_init(&cap_domain, 1, parts);
	k_mem_domain_add_thread(&cap_domain, ta);
	k_mem_domain_add_thread(&cap_domain, tb);
	k_thread_access_grant(ta, &go_a_use, &done_a_use, &go_a_delegate,
			      &done_a_deleg);
	k_thread_access_grant(tb, &go_b_use, &done_b_use, &go_b_again,
			      &done_b_again);
#endif

	/* Mint a capability owned by A. */
	the_cap = cap_mint(CAP_RES_SENSOR, ta);

	k_thread_start(ta);
	k_thread_start(tb);

	/* Drive the sequence. */
	k_sem_give(&go_a_use);      k_sem_take(&done_a_use, K_FOREVER);
	k_sem_give(&go_a_delegate); k_sem_take(&done_a_deleg, K_FOREVER);
	k_sem_give(&go_b_use);      k_sem_take(&done_b_use, K_FOREVER);

	printk("\n[sys] (6) revoke the capability:\n");
	cap_revoke(the_cap);

	k_sem_give(&go_b_again);    k_sem_take(&done_b_again, K_FOREVER);

	printk("\n*** done: used / forge-blocked / delegated / holder-checked / revoked ***\n");
	return 0;
}
