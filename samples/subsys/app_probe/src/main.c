/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shared service library probe. A loadable app is verified, loaded and run
 * isolated, granted a set of capabilities, and calls the platform's shared
 * services (time, random, crypto). The services, their capability checks and
 * their syscalls all live in the platform, so the app links none of it. This
 * shows the shared library is reachable and capability-gated from a downloadable
 * app.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t probe_app[] __aligned(8) = {
#include "probe_app.inc"
};
static const uint8_t probe_app_hash[32] = {
#include "probe_app.sha256.inc"
};

int main(void)
{
	printk("\n*** shared service library probe (llext, C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load("probe", probe_app, ARRAY_SIZE(probe_app),
					probe_app_hash, "app_main", &ext, &entry);

	if (rc != 0) {
		printk("load failed: %d\n", rc);
		return 0;
	}

	app_caps_t caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_RANDOM |
			APP_CAP_CRYPTO | APP_CAP_SENSOR | APP_CAP_KV |
			APP_CAP_FLASH | APP_CAP_GPIO | APP_CAP_IPC |
			APP_CAP_INSPECT | APP_CAP_PWM | APP_CAP_ADC |
			APP_CAP_I2C | APP_CAP_SPI | APP_CAP_UART |
			APP_CAP_BLE | APP_CAP_RADIO |
			APP_CAP_CAN | APP_CAP_DMA;

	/*
	 * Grant only line 0 before launch (the first app takes group id 0), so
	 * the probe sees line 0 allowed and line 1 refused regardless of when
	 * its thread is first scheduled.
	 */
	app_platform_grant_gpio(0, BIT(0));
	app_platform_grant_pwm(0, BIT(0));
	app_platform_grant_adc(0, BIT(0));
	(void)app_platform_grant_i2c(0, 0x50);
	app_platform_grant_spi(0, true);
	/* The last 8 KB of the storage partition, clear of the registry and
	 * the module blobs that live at its start.
	 */
	app_platform_grant_flash(0, 184 * 1024, 8 * 1024);

	app_platform_launch_llext("probe", ext, (app_entry_t)entry, NULL, caps);

	k_msleep(25000);

	printk("\n*** done: a loadable app used the shared service library ***\n");
	return 0;
}
