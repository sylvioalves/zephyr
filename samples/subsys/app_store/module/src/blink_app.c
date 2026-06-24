/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable "app" module. In a full system this would be fetched over the
 * network, verified, stored, and installed at runtime. Here it is built as an
 * llext extension and embedded, but the install path is identical: verify, load,
 * resolve the entry symbol, run.
 */

#include <zephyr/llext/symbol.h>
#include <zephyr/sys/printk.h>

void app_main(void)
{
	printk("    [installed app] hello from a module loaded at runtime!\n");
	for (int i = 0; i < 3; i++) {
		printk("    [installed app] tick %d\n", i);
	}
}
EXPORT_SYMBOL(app_main);
