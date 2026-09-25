/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_PLATFORM_DEVICES_DISPLAY_CAPTURE_H_
#define APP_PLATFORM_DEVICES_DISPLAY_CAPTURE_H_

#include <zephyr/devicetree.h>
#include <stdint.h>

#define CAPTURE_NODE DT_CHOSEN(zephyr_display)
#define CAPTURE_W    DT_PROP(CAPTURE_NODE, width)
#define CAPTURE_H    DT_PROP(CAPTURE_NODE, height)

extern uint16_t capture_fb[CAPTURE_H][CAPTURE_W];
extern uint32_t capture_writes;

/* Report a refresh to the registered event callback, as a panel would. */
void capture_vsync(void);

#endif /* APP_PLATFORM_DEVICES_DISPLAY_CAPTURE_H_ */
