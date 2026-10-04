/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Launch as many parallel loadable applications as the platform allows. Each
 * is an independent instance of the same verified llext module, loaded and run
 * as an isolated user-mode app that loops with a one-second period. The number
 * of instances is CONFIG_APP_PLATFORM_MAX_APPS; this sample reports how many
 * actually started so the concurrent-app ceiling can be measured.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/llext/llext.h>
#include <zephyr/multi_heap/shared_multi_heap.h>

static const uint8_t hello_app[] __aligned(8) = {
#include "hello.inc"
};
static const uint8_t hello_hash[32] = {
#include "hello.sha256.inc"
};

#define APP_CAPS (APP_CAP_LOG | APP_CAP_TIME)
#define N_APPS   CONFIG_APP_PLATFORM_MAX_APPS

/*
 * Size of the loadable-extension heap to carve from external RAM. Holding the
 * loaded modules' code and data here keeps them out of the limited internal
 * SRAM, so many more apps fit. Only the modules live in PSRAM; their thread
 * stacks stay in internal SRAM, where the trap path needs them.
 */
#define LLEXT_PSRAM_HEAP (512 * 1024)

int main(void)
{
	printk("\n*** many parallel loadable apps (C5) ***\n");

#ifdef CONFIG_LLEXT_HEAP_DYNAMIC
	void *heap = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 64,
						     LLEXT_PSRAM_HEAP);

	if (heap == NULL || llext_heap_init(heap, LLEXT_PSRAM_HEAP) != 0) {
		printk("llext PSRAM heap init failed\n");
		return 0;
	}
	printk("llext heap: %u KB in external RAM at %p\n",
	       LLEXT_PSRAM_HEAP / 1024, heap);
#endif

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	int launched = 0;

	for (int i = 0; i < N_APPS; i++) {
		char name[12];

		name[0] = 'a'; name[1] = 'p'; name[2] = 'p';
		name[3] = '0' + (i / 10);
		name[4] = '0' + (i % 10);
		name[5] = '\0';

		struct llext *ext = NULL;
		void *entry = NULL;
		int rc = app_loader_verify_load(name, hello_app,
						ARRAY_SIZE(hello_app), hello_hash,
						"app_main", &ext, &entry);

		if (rc != 0) {
			printk("load #%d failed: %d\n", i, rc);
			break;
		}

		rc = app_platform_launch_llext(name, ext, (app_entry_t)entry, NULL,
					       APP_CAPS);
		if (rc < 0) {
			printk("launch #%d failed: %d\n", i, rc);
			break;
		}
		launched++;
	}

	printk("*** launched %d / %d parallel loadable apps ***\n", launched,
	       N_APPS);

	k_msleep(8000);

	printk("*** %d apps alive after 8 s ***\n", launched);
	return 0;
}
