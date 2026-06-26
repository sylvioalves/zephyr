/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using a hardware DMA memory copy through services from user mode. The
 * platform owns the GDMA channel and the DMA-capable buffers; the app hands over
 * a pattern to copy and checks the result came back intact, never touching the
 * DMA engine directly.
 */

#include "svc.h"

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

	for (int round = 0; round < 3; round++) {
		uint8_t in[32];
		uint8_t out[32] = { 0 };

		for (int i = 0; i < (int)sizeof(in); i++) {
			in[i] = (uint8_t)(round * 16 + i);
		}

		int rc = svc_dma_copy(in, out, sizeof(in));
		bool match = (rc == (int)sizeof(in)) &&
			     (memcmp(in, out, sizeof(in)) == 0);

		printk("  [app] round %d: dma copy rc=%d, first=%02x last=%02x, match=%d\n",
		       round, rc, out[0], out[sizeof(out) - 1], match);

		k_msleep(200);
	}

	printk("  [app] done - copied buffers via hardware DMA\n");
}

int main(void)
{
	printk("\n*** more services 13: hardware DMA copy (C6) ***\n");

	int rc = svc_dma_install();

	printk("  [platform] dma channel install rc=%d\n", rc);

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
