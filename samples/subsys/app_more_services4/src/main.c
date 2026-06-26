/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An app using the SPI bus and system hardware info through services. It runs a
 * SPI transfer (the controller is platform-owned; wire MISO to MOSI to see a
 * loopback), reads the chip's unique device id, and reads why the device last
 * reset. All reached only through service syscalls from user mode.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

#define STACK_SIZE 2048
#define APP_PRIO   5

K_THREAD_STACK_DEFINE(app_stack, STACK_SIZE);
static struct k_thread app_thread;

static void app_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* SPI: full-duplex transfer on the granted chip-select (CS 0). */
	uint8_t tx[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t rx[4] = { 0 };
	int rc = svc_spi_xfer(0, tx, rx, sizeof(tx));

	if (rc >= 0) {
		printk("  [app] SPI cs0 sent DE AD BE EF, received %02x %02x %02x %02x\n",
		       rx[0], rx[1], rx[2], rx[3]);
	} else {
		printk("  [app] SPI cs0 xfer rc=%d\n", rc);
	}

	/* SPI: chip-select 1 was NOT granted - rejected at the boundary. */
	int rc1 = svc_spi_xfer(1, tx, rx, sizeof(tx));

	printk("  [app] SPI cs1 xfer -> rc=%d (expected -EPERM=%d, ungranted)\n",
	       rc1, -EPERM);

	/* hwinfo: unique device id. */
	uint8_t id[8] = { 0 };
	int n = svc_device_id(id, sizeof(id));

	printk("  [app] device id (%d bytes): ", n);
	for (int i = 0; i < n; i++) {
		printk("%02x", id[i]);
	}
	printk("\n");

	/* hwinfo: reset cause. */
	uint32_t cause = 0;

	if (svc_reset_cause(&cause) == 0) {
		printk("  [app] last reset cause = 0x%08x\n", cause);
	}

	printk("  [app] done - used SPI + device id + reset cause\n");
}

int main(void)
{
	printk("\n*** more services 4: SPI + system info (C6) ***\n");

	/* Grant the app chip-select 0 only; chip-select 1 stays denied. */
	svc_spi_grant(BIT(0));

	k_tid_t t = k_thread_create(&app_thread, app_stack, STACK_SIZE, app_entry,
				    NULL, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);

	k_thread_start(t);
	k_thread_join(t, K_FOREVER);

	printk("\n*** done ***\n");
	return 0;
}
