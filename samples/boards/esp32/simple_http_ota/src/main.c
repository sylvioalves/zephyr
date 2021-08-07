/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr.h>
#include <sys/printk.h>
#include <logging/log.h>
LOG_MODULE_REGISTER(main);

#include "simple_http_ota.h"
#include "dhcp.h"

void main(void)
{
	int ret = -1;

	LOG_INF("OTA sample app started");

	ret = simple_http_ota_init();

	LOG_INF("Waiting IP address from network..");
	app_dhcpv4_startup();

	if (ret < 0) {
		LOG_ERR("Failed to init esp http ota application");
	}
}
