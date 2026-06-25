/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Per-app sandboxed filesystem. The app reads and writes files in its own
 * private namespace as if it had a whole filesystem, but it is confined to that
 * subtree: a path that tries to escape (absolute, or containing "..") is denied
 * by the broker before any real filesystem access. This is the namespaced
 * storage capability - one shared filesystem, private per-app views.
 */

#include "fs_svc.h"
#include "fs_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	printk("\n=== app uses its own private storage ===\n");

	/* Write a file in the app's own namespace. */
	int h = app_file_open("config.txt", 1);

	if (h >= 0) {
		const char *data = "brightness=80\n";

		app_file_write(h, data, strlen(data));
		app_file_close(h);
		printk("  [app] wrote config.txt\n");
	}

	/* Read it back. */
	h = app_file_open("config.txt", 0);
	if (h >= 0) {
		char buf[32] = {0};
		int n = app_file_read(h, buf, sizeof(buf) - 1);

		app_file_close(h);
		printk("  [app] read config.txt (%d bytes): %s", n, buf);
	}

	/* Now try to ESCAPE the sandbox - these must all be denied. */
	printk("\n=== app tries to escape its sandbox (must be denied) ===\n");

	printk("  [app] try '../1/secret.txt' (another app's dir)...\n");
	int rc = app_file_open("../1/secret.txt", 0);

	printk("  [app] -> rc=%d (%s)\n", rc, rc < 0 ? "DENIED" : "LEAKED!");

	printk("  [app] try '/lfs/system/keys' (absolute, system path)...\n");
	rc = app_file_open("/lfs/system/keys", 0);
	printk("  [app] -> rc=%d (%s)\n", rc, rc < 0 ? "DENIED" : "LEAKED!");

	printk("  [app] try 'a/../../etc' (sneaky traversal)...\n");
	rc = app_file_open("a/../../etc", 0);
	printk("  [app] -> rc=%d (%s)\n", rc, rc < 0 ? "DENIED" : "LEAKED!");
}

int main(void)
{
	printk("\n*** per-app sandboxed filesystem (C6) ***\n");

	if (fs_broker_init() != 0) {
		printk("fs_broker_init failed\n");
		return 0;
	}

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	/* This app gets namespace 0: /lfs/apps/0/. */
	fs_broker_register(t);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done: app used its private storage; all escapes denied ***\n");
	return 0;
}
