/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <profile_api.h>

#include <zephyr/kernel.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>

static const uint8_t extension_llext[] __aligned(4) = {
#include <extension.inc>
};

int profile_scale(int value)
{
	return value * 3;
}
EXPORT_SYMBOL(profile_scale);

int main(void)
{
	struct llext_buf_loader buf_loader =
		LLEXT_BUF_LOADER(extension_llext, sizeof(extension_llext));
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;
	int (*start_fn)(int value);
	int ret;

	ret = llext_load(&buf_loader.loader, "extension", &ext, &ldr_parm);
	if (ret != 0) {
		printk("Loading the extension failed: %d\n", ret);
		return 0;
	}

	start_fn = llext_find_sym(&ext->exp_tab, "start");
	if (start_fn == NULL) {
		printk("The extension has no start symbol\n");
		return 0;
	}

	printk("start(14) is %d\n", start_fn(14));
	llext_unload(&ext);
	printk("Done\n");

	return 0;
}
