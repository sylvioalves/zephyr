/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app playing audio through an I2S service from user mode. The platform owns
 * the I2S controller, its DMA, and the transmit slab; the app only generates PCM
 * samples and hands them over. Here it synthesizes a short sine-like tone and
 * streams several blocks, never touching the I2S device or its kernel objects.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define STACK_SIZE  4096
#define APP_PRIO    5
#define SAMPLES     256

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

/* A tiny triangle wave: cheap to compute in user mode, no math library. */
static void fill_tone(int16_t *buf, int n_frames, int step)
{
	int16_t v = 0;

	for (int i = 0; i < n_frames; i++) {
		v = (int16_t)((i * step) & 0x3fff);
		buf[2 * i] = v;       /* left  */
		buf[2 * i + 1] = v;   /* right */
	}
}

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	int16_t pcm[SAMPLES * 2];

	for (int round = 0; round < 4; round++) {
		fill_tone(pcm, SAMPLES, 64 * (round + 1));

		int rc = svc_audio_play((const uint8_t *)pcm, sizeof(pcm));

		printk("  [app] round %d: played %d bytes (rc=%d)\n",
		       round, (int)sizeof(pcm), rc);
	}

	printk("  [app] done - streamed PCM through I2S service\n");
}

int main(void)
{
	printk("\n*** more services 10: I2S audio (C6) ***\n");

	int rc = svc_audio_install();

	printk("  [platform] i2s configure rc=%d\n", rc);

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
