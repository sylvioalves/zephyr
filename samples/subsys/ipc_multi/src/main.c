/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Three isolated llext apps messaging each other. Each is verified and launched
 * into its own memory domain; they reach each other only through the platform's
 * capability-gated messaging services. This shows directed-by-name delivery and
 * topic publish/subscribe working across separate apps, not an app talking to
 * itself.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const uint8_t consumer_app[] __aligned(8) = {
#include "consumer.inc"
};
static const uint8_t consumer_hash[32] = {
#include "consumer.sha256.inc"
};
static const uint8_t logger_app[] __aligned(8) = {
#include "logger.inc"
};
static const uint8_t logger_hash[32] = {
#include "logger.sha256.inc"
};
static const uint8_t producer_app[] __aligned(8) = {
#include "producer.inc"
};
static const uint8_t producer_hash[32] = {
#include "producer.sha256.inc"
};

static int install(const char *name, const uint8_t *buf, size_t len,
		   const uint8_t hash[32])
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load(name, buf, len, hash, "app_main",
					&ext, &entry);

	if (rc != 0) {
		printk("%s of %s failed: %d\n", __func__, name, rc);
		return rc;
	}
	return app_platform_launch_llext(name, ext, (app_entry_t)entry, NULL,
					 APP_CAP_LOG | APP_CAP_IPC);
}

int main(void)
{
	printk("\n*** three isolated llext apps messaging (C6) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	/* Receivers first so they register and subscribe before the producer. */
	install("consumer", consumer_app, ARRAY_SIZE(consumer_app), consumer_hash);
	install("logger", logger_app, ARRAY_SIZE(logger_app), logger_hash);
	install("producer", producer_app, ARRAY_SIZE(producer_app), producer_hash);

	k_msleep(5000);

	printk("\n*** done: apps exchanged directed and published messages ***\n");
	return 0;
}
