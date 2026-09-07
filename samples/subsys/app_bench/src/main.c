/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Syscall boundary and PMP context switch micro-benchmark. */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/arch/riscv/csr.h>

#define N_CALL  100000U
#define N_SHA   10000U
#define N_PING  20000U

#ifndef BENCH_POLL_MS
#define BENCH_POLL_MS 100
#endif

static inline uint32_t read_cycles(void)
{
	uint32_t c;

	__asm__ volatile("rdcycle %0" : "=r"(c));
	return c;
}

K_APPMEM_PARTITION_DEFINE(res_part);

struct bench_res {
	uint32_t empty;
	uint32_t hb;
	uint32_t log;
	uint32_t sha;
	uint32_t sha_denied;
	int sha_rc;
	int denied_rc;
	volatile int done;
};

K_APP_DMEM(res_part) struct bench_res ures;
static K_SEM_DEFINE(sem_ready, 0, 1);

static void wait_done(int v)
{
#ifdef BENCH_SEM
	k_sem_take(&sem_ready, K_FOREVER);
	ARG_UNUSED(v);
#else
	while (ures.done != v) {
		k_msleep(BENCH_POLL_MS);
	}
#endif
}

static uint32_t loop_empty(uint32_t n)
{
	volatile uint32_t sink = 0;
	uint32_t t0 = read_cycles();

	for (uint32_t i = 0; i < n; i++) {
		sink++;
	}
	return read_cycles() - t0;
}

static void bench_user(void *arg)
{
	uint8_t in[32] = {0};
	uint8_t out[32];
	uint32_t t0;
	int rc = 0;

	ARG_UNUSED(arg);

	ures.empty = loop_empty(N_CALL);

	t0 = read_cycles();
	for (uint32_t i = 0; i < N_CALL; i++) {
		svc_heartbeat();
	}
	ures.hb = read_cycles() - t0;

	t0 = read_cycles();
	for (uint32_t i = 0; i < N_CALL; i++) {
		svc_log("x");
	}
	ures.log = read_cycles() - t0;

	t0 = read_cycles();
	for (uint32_t i = 0; i < N_SHA; i++) {
		rc |= svc_sha256(in, sizeof(in), out);
	}
	ures.sha = read_cycles() - t0;
	ures.sha_rc = rc;
	ures.done = 1;
#ifdef BENCH_SEM
	k_sem_give(&sem_ready);
#endif
}

static void bench_denied(void *arg)
{
	uint8_t in[32] = {0};
	uint8_t out[32];
	uint32_t t0;
	int rc = 0;

	ARG_UNUSED(arg);

	t0 = read_cycles();
	for (uint32_t i = 0; i < N_CALL; i++) {
		rc = svc_sha256(in, sizeof(in), out);
	}
	ures.sha_denied = read_cycles() - t0;
	ures.denied_rc = rc;
	ures.done = 2;
#ifdef BENCH_SEM
	k_sem_give(&sem_ready);
#endif
}

static void report(const char *tag, uint32_t cyc, uint32_t n)
{
	uint32_t per = cyc / n;

	printk("%-28s %10u cyc total  %6u cyc/call  %5u ns/call\n", tag, cyc,
	       per, (per * 1000U) / 240U);
}

/*
 * Direct z_impl calls from the main kernel thread, which owns no application
 * identity: only the services that carry no capability gate report a figure
 * here. The gated ones are measured from inside an application above, where
 * the identity is the kernel-owned thread pointer.
 */
static void run_kernel_direct(void)
{
	uint32_t t0, c;

	c = loop_empty(N_CALL);
	report("kernel empty loop", c, N_CALL);

	t0 = read_cycles();
	for (uint32_t i = 0; i < N_CALL; i++) {
		z_impl_svc_heartbeat();
	}
	report("kernel direct heartbeat", read_cycles() - t0, N_CALL);
}

/* ---- 1.4: context switch ping-pong ---- */

K_APPMEM_PARTITION_DEFINE(p0);
K_APPMEM_PARTITION_DEFINE(p1);
K_APPMEM_PARTITION_DEFINE(p2);
K_APPMEM_PARTITION_DEFINE(p3);
K_APPMEM_PARTITION_DEFINE(p4);
K_APPMEM_PARTITION_DEFINE(p5);
K_APPMEM_PARTITION_DEFINE(p6);
K_APP_DMEM(p0) volatile uint32_t d0;
K_APP_DMEM(p1) volatile uint32_t d1;
K_APP_DMEM(p2) volatile uint32_t d2;
K_APP_DMEM(p3) volatile uint32_t d3;
K_APP_DMEM(p4) volatile uint32_t d4;
K_APP_DMEM(p5) volatile uint32_t d5;
K_APP_DMEM(p6) volatile uint32_t d6;

static struct k_mem_partition *all_parts[] = {&p0, &p1, &p2, &p3, &p4, &p5, &p6};

static K_SEM_DEFINE(sem_a, 0, 1);
static K_SEM_DEFINE(sem_b, 0, 1);
static K_SEM_DEFINE(sem_done, 0, 1);
static K_THREAD_STACK_DEFINE(stk_a, 2048);
static K_THREAD_STACK_DEFINE(stk_b, 2048);
static struct k_thread th_a, th_b;
static struct k_mem_domain dom_a, dom_b;

static void ping(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (uint32_t i = 0; i < N_PING; i++) {
		k_sem_give(&sem_b);
		k_sem_take(&sem_a, K_FOREVER);
	}
	k_sem_give(&sem_done);
}

static void pong(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (uint32_t i = 0; i < N_PING; i++) {
		k_sem_take(&sem_b, K_FOREVER);
		k_sem_give(&sem_a);
	}
}

static void pingpong(bool user, int nparts)
{
	uint32_t t0, c;
	uint32_t opts = user ? K_USER : 0;

	k_sem_reset(&sem_a);
	k_sem_reset(&sem_b);
	k_sem_reset(&sem_done);

	k_thread_create(&th_a, stk_a, K_THREAD_STACK_SIZEOF(stk_a), ping, NULL,
			NULL, NULL, 3, opts, K_FOREVER);
	k_thread_create(&th_b, stk_b, K_THREAD_STACK_SIZEOF(stk_b), pong, NULL,
			NULL, NULL, 3, opts, K_FOREVER);
	if (user) {
		k_mem_domain_init(&dom_a, nparts, all_parts);
		k_mem_domain_init(&dom_b, nparts, all_parts);
		k_mem_domain_add_thread(&dom_a, &th_a);
		k_mem_domain_add_thread(&dom_b, &th_b);
		k_thread_access_grant(&th_a, &sem_a, &sem_b, &sem_done);
		k_thread_access_grant(&th_b, &sem_a, &sem_b);
	}
	t0 = read_cycles();
	k_thread_start(&th_a);
	k_thread_start(&th_b);
	k_sem_take(&sem_done, K_FOREVER);
	c = read_cycles() - t0;
	k_thread_join(&th_a, K_FOREVER);
	k_thread_join(&th_b, K_FOREVER);

	uint32_t per = c / (2U * N_PING);

	printk("%s %-6s parts=%d: %10u cyc, %u switches, %5u cyc/switch %5u ns\n", __func__,
	       user ? "user" : "kernel", nparts, c, 2U * N_PING, per,
	       (per * 1000U) / 240U);
}

int main(void)
{
	printk("\n*** app_bench: syscall boundary cost (C5 @240 MHz, rdcycle) ***\n");
	printk("N_CALL=%u N_SHA=%u N_PING=%u\n", N_CALL, N_SHA, N_PING);

	/* Let U-mode read the cycle counter (mcounteren.CY). */
	csr_write(mcounteren, 0x7);
	k_thread_access_grant(k_current_get(), &sem_ready);
	printk("variant: %s poll=%d ms\n", IS_ENABLED(BENCH_SEM) ? "sem" : "poll", BENCH_POLL_MS);

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	const struct app_def crypto_app = {
		.name = "bench0", .source = APP_BUILTIN, .entry = bench_user,
		.caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_CRYPTO,
	};
	const struct app_def denied_app = {
		.name = "bench1", .source = APP_BUILTIN, .entry = bench_denied,
		.caps = APP_CAP_LOG | APP_CAP_TIME,
	};

	int id0 = app_platform_launch(&crypto_app);

	k_mem_domain_add_partition(app_platform_domain(id0), &res_part);
	wait_done(1);
	printk("-- user-mode app (bench0, caps LOG|TIME|CRYPTO)\n");
	report("user empty loop", ures.empty, N_CALL);
	report("user svc_heartbeat()", ures.hb, N_CALL);
	report("user svc_log(\"x\")", ures.log, N_CALL);
	report("user svc_sha256(32B)", ures.sha, N_SHA);
	printk("  (sha rc=%d)\n", ures.sha_rc);

	int id1 = app_platform_launch(&denied_app);

	k_mem_domain_add_partition(app_platform_domain(id1), &res_part);
	wait_done(2);
	printk("-- user-mode app (bench1, caps LOG|TIME, no CRYPTO)\n");
	report("user svc_sha256 denied", ures.sha_denied, N_CALL);
	printk("  (denied rc=%d, expect -EPERM=-1)\n", ures.denied_rc);

	printk("-- kernel thread (%s), direct z_impl calls\n", __func__);
	run_kernel_direct();

	printk("-- 1.4 context switch ping-pong on k_sem\n");
	pingpong(false, 0);
	pingpong(true, 0);
	pingpong(true, 2);
	pingpong(true, 4);
	pingpong(true, 7);

	printk("*** app_bench done ***\n");
	return 0;
}
