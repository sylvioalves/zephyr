/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using crypto and a persistent key-value store. It hashes a string with
 * SHA-256, then stores a boot counter in its key-value namespace, reads it back,
 * and increments it - so the value survives reboots. Both are reached only
 * through service syscalls from user mode.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

extern void svc_init(void);

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* crypto: hash a string. */
	const char *msg = "hello platform";
	uint8_t digest[32];

	if (svc_sha256((const uint8_t *)msg, strlen(msg), digest) == 0) {
		printk("  [app] sha256(\"%s\") = %02x%02x%02x%02x...\n",
		       msg, digest[0], digest[1], digest[2], digest[3]);
	}

	/* key-value: read a persistent boot counter, increment, store. */
	uint32_t boots = 0;
	int rc = svc_kv_get("boots", &boots);

	if (rc == 0) {
		printk("  [app] persistent boot counter = %u (loaded from KV)\n",
		       boots);
	} else {
		printk("  [app] no boot counter yet (first run)\n");
	}

	boots++;
	svc_kv_set("boots", boots);
	printk("  [app] stored boot counter = %u (will survive reboot)\n", boots);

	printk("  [app] done - used crypto + persistent key-value store\n");
}

int main(void)
{
	printk("\n*** more services 2: crypto + persistent KV (C6) ***\n");
	svc_init();

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** reset the board: the boot counter advances ***\n");
	return 0;
}
