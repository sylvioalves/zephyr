/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PLATFORM_DEVICES_SPI_ECHO_H_
#define APP_PLATFORM_DEVICES_SPI_ECHO_H_

#include <zephyr/drivers/spi.h>

/* The configuration of the last transfer the echo device at @p cs took, or
 * NULL when it took none.
 */
const struct spi_config *spi_echo_last(int cs);

#endif /* APP_PLATFORM_DEVICES_SPI_ECHO_H_ */
