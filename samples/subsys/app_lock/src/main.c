/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Lockup isolation experiment. MODE: 1 = U-mode busy loop, no syscalls;
 * 2 = plain K_USER thread ecall loop; 3 = platform app ecall loop;
 * 4 = kernel thread calling the syscall wrapper (ecall from M-mode).
 * RDC: 1 = read rdcycle in the loop. main polls every POLL_MS.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/arch/riscv/csr.h>

#ifndef MODE
#define MODE 3
#endif
/* The ESP32-C6 core has no standard counter CSRs. */
#ifndef RDC
#define RDC (!IS_ENABLED(CONFIG_SOC_SERIES_ESP32C6))
#endif
#ifndef POLL_MS
#define POLL_MS 1
#endif
#define N 100000U


static void body(void *arg)
{
	volatile uint32_t sink = 0;
	uint32_t t = 0;

	ARG_UNUSED(arg);
	for (uint32_t r = 0; r < 20; r++) {
		for (uint32_t i = 0; i < N; i++) {
#if RDC
			__asm__ volatile("rdcycle %0" : "=r"(t));
#endif
#if MODE == 1
			sink += t;
#elif MODE == 5
			k_yield();
			sink += t;
#else
			app_svc_heartbeat();
			sink += t;
#endif
		}
	}
}

static __maybe_unused K_THREAD_STACK_DEFINE(stk, 2048);
static __maybe_unused K_THREAD_STACK_DEFINE(stk2, 2048);
static __maybe_unused struct k_thread th2;

/* Second thread: sleeps POLL_MS so the timer ISR reschedules to it. */
static __maybe_unused void sleeper(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
		k_msleep(POLL_MS);
	}
}
static __maybe_unused struct k_timer tick;
static K_SEM_DEFINE(never, 0, 1);
static volatile uint32_t ticks;

static __maybe_unused void tick_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	ticks++;
}
static __maybe_unused struct k_thread th;

int main(void)
{
	printk("\n*** app_lock MODE=%d RDC=%d POLL_MS=%d NOCNT=%d ***\n",
	       MODE, RDC, POLL_MS, IS_ENABLED(NOCNT));
#if RDC && !defined(NOCNT)
	csr_write(mcounteren, 0x7);
#endif
	if (app_platform_init() != 0) {
		return 0;
	}
#if MODE == 3
	const struct app_def a = {
		.name = "lock", .source = APP_BUILTIN, .entry = body,
		.caps = APP_CAP_LOG | APP_CAP_TIME,
	};
	app_platform_launch(&a);
#elif MODE == 4
	k_thread_create(&th, stk, K_THREAD_STACK_SIZEOF(stk), (k_thread_entry_t)body,
			NULL, NULL, NULL, 5, 0, K_NO_WAIT);
#elif !defined(TWO_BUSY)
	k_thread_create(&th, stk, K_THREAD_STACK_SIZEOF(stk), (k_thread_entry_t)body,
			NULL, NULL, NULL, 5, K_USER, K_NO_WAIT);
#endif
	uint32_t last = 0;
	int64_t start;
	int64_t now;

#ifdef TWO_BUSY
	/* Two busy user threads, equal priority, timeslice reschedules them. */
	k_thread_create(&th, stk, K_THREAD_STACK_SIZEOF(stk), (k_thread_entry_t)body,
			NULL, NULL, NULL, 2, K_USER, K_NO_WAIT);
	k_thread_create(&th2, stk2, K_THREAD_STACK_SIZEOF(stk2), (k_thread_entry_t)body,
			NULL, NULL, NULL, 2, K_USER, K_NO_WAIT);
	for (uint32_t i = 0; i < 8; i++) {
		k_msleep(1000);
		printk("alive %u s\n", last++);
	}
	printk("*** app_lock survived 8 s, no lockup ***\n");
	return 0;
#endif
#ifdef SLEEPER_USER
	k_thread_create(&th2, stk2, K_THREAD_STACK_SIZEOF(stk2), sleeper, NULL,
			NULL, NULL, 4, K_USER, K_NO_WAIT);
#endif
#ifdef SLEEPER_KERNEL
	k_thread_create(&th2, stk2, K_THREAD_STACK_SIZEOF(stk2), sleeper, NULL,
			NULL, NULL, 4, 0, K_NO_WAIT);
#endif
#if defined(SLEEPER_USER) || defined(SLEEPER_KERNEL)
	/* main only wakes once a second. */
	for (uint32_t i = 0; i < 8; i++) {
		k_msleep(1000);
		printk("alive %u s\n", last++);
	}
	printk("*** app_lock survived 8 s, no lockup ***\n");
	return 0;
#endif

#ifdef TIMER_ONLY
	/* Periodic interrupt with no reschedule: main never wakes. */
	k_timer_init(&tick, tick_fn, NULL);
	k_timer_start(&tick, K_MSEC(POLL_MS), K_MSEC(POLL_MS));
	k_sem_take(&never, K_FOREVER);
#endif
	start = k_uptime_get();
	while ((now = k_uptime_get() - start) < 8000) {
		if (now >= (int64_t)last * 1000) {
			printk("alive %u s\n", last++);
		}
		k_msleep(POLL_MS);
	}
	printk("*** app_lock survived 8 s, no lockup ***\n");
	return 0;
}
