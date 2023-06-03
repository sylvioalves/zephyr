/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gpio.h>

#define BL_PIN     13 // gpio1 45

static int board_esp32s3_box_lite_init(void)
{
	const struct device *gpio1;

	gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	if (!device_is_ready(gpio1)) {
		return -ENODEV;
	}

	/* turns LCD backlight on */
	gpio_pin_configure(gpio1, BL_PIN, GPIO_OUTPUT);
	gpio_pin_set(gpio1, BL_PIN, 0);

	return 0;
}

SYS_INIT(board_esp32s3_box_lite_init, APPLICATION, CONFIG_GPIO_INIT_PRIORITY);
