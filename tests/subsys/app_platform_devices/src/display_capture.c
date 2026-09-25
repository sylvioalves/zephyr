/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A display that keeps its RGB565 frame in RAM, so the test can read back what
 * the display service wrote, count the driver calls it made and report
 * refreshes when it chooses.
 */

#define DT_DRV_COMPAT vnd_display_capture

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <errno.h>
#include <string.h>

#include "display_capture.h"

uint16_t capture_fb[CAPTURE_H][CAPTURE_W];
uint32_t capture_writes;

static display_event_cb_t capture_cb;
static void *capture_user_data;

static int capture_write(const struct device *dev, const uint16_t x, const uint16_t y,
			 const struct display_buffer_descriptor *desc, const void *buf)
{
	const uint16_t *src = buf;

	ARG_UNUSED(dev);

	if ((uint32_t)x + desc->width > CAPTURE_W || (uint32_t)y + desc->height > CAPTURE_H ||
	    desc->pitch < desc->width) {
		return -EINVAL;
	}
	for (uint16_t r = 0U; r < desc->height; r++) {
		memcpy(&capture_fb[y + r][x], &src[r * desc->pitch],
		       desc->width * sizeof(uint16_t));
	}
	capture_writes++;
	return 0;
}

static void capture_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	ARG_UNUSED(dev);

	caps->x_resolution = CAPTURE_W;
	caps->y_resolution = CAPTURE_H;
	caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	caps->current_pixel_format = PIXEL_FORMAT_RGB_565;
	caps->supported_events = DISPLAY_EVENT_VSYNC;
}

static int capture_register_event_cb(const struct device *dev, display_event_cb_t cb,
				     void *user_data, uint32_t event_mask, bool in_isr,
				     uint32_t *out_reg_handle)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(event_mask);
	ARG_UNUSED(in_isr);

	capture_cb = cb;
	capture_user_data = user_data;
	*out_reg_handle = 0U;
	return 0;
}

void capture_vsync(void)
{
	const struct display_event_data evt = { 0 };

	if (capture_cb != NULL) {
		(void)capture_cb(DEVICE_DT_INST_GET(0), DISPLAY_EVENT_VSYNC, &evt,
				 capture_user_data);
	}
}

static DEVICE_API(display, capture_api) = {
	.write = capture_write,
	.get_capabilities = capture_get_capabilities,
	.register_event_cb = capture_register_event_cb,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,
		      &capture_api);
