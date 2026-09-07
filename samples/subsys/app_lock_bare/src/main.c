/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Minimal lockup repro: two busy user threads under timeslicing. */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#ifndef YIELD
#define YIELD 0
#endif

static void body(void *a, void *b, void *c)
{
	volatile uint32_t sink = 0;

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	for (;;) {
#if YIELD
		k_yield();
#endif
		sink++;
	}
}

static K_THREAD_STACK_DEFINE(stk, 2048);
static K_THREAD_STACK_DEFINE(stk2, 2048);
static struct k_thread th, th2;

int main(void)
{
	printk("\n*** app_lock_bare YIELD=%d ***\n", YIELD);
	k_thread_create(&th, stk, K_THREAD_STACK_SIZEOF(stk), body, NULL, NULL,
			NULL, 2, K_USER, K_NO_WAIT);
	k_thread_create(&th2, stk2, K_THREAD_STACK_SIZEOF(stk2), body, NULL,
			NULL, NULL, 2, K_USER, K_NO_WAIT);
	for (uint32_t i = 0; i < 8; i++) {
		k_msleep(1000);
		printk("alive %u s\n", i);
	}
	printk("*** survived 8 s, no lockup ***\n");
	return 0;
}
