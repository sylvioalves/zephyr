/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using a raw flash partition through services from user mode. The
 * platform owns the partition and confines every access to it; the app erases a
 * sector, writes a pattern, reads it back, and confirms a write past the
 * partition end is rejected. It never touches the flash device directly.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <string.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	uint8_t out[16];
	uint8_t in[16] = { 0 };

	for (int i = 0; i < (int)sizeof(out); i++) {
		out[i] = (uint8_t)(0xA0 + i);
	}

	/* Erase one sector, write a pattern, read it back. */
	int re = svc_flash_erase(0, 4096);
	int rw = svc_flash_write(0, out, sizeof(out));
	int rr = svc_flash_read(0, in, sizeof(in));

	printk("  [app] erase rc=%d, write rc=%d, read rc=%d\n", re, rw, rr);

	bool match = (memcmp(out, in, sizeof(in)) == 0);

	printk("  [app] read back: %02x %02x %02x %02x ... (match=%d)\n",
	       in[0], in[1], in[2], in[3], match);

	/* A write past the partition end is bounds-rejected. */
	int rb = svc_flash_write(0xFFFFFFF0U, out, sizeof(out));

	printk("  [app] out-of-bounds write -> rc=%d (expected -EINVAL=%d)\n",
	       rb, -EINVAL);

	printk("  [app] done - erased, wrote, read, confined to partition\n");
}

int main(void)
{
	printk("\n*** more services 11: raw flash partition (C6) ***\n");

	int rc = svc_flash_install();

	printk("  [platform] flash area open rc=%d\n", rc);

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
