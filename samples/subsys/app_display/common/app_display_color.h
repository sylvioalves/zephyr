/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_DISPLAY_COLOR_H_
#define APP_DISPLAY_COLOR_H_

#include <zephyr/drivers/display.h>
#include <stdint.h>

/* Tint of each application, indexed by the argument it is launched with. */
static const uint8_t app_display_tint[2][3] = {
	{ 0U, 160U, 255U },
	{ 255U, 140U, 0U },
};

/* Pack an RGB color into a pixel of the panel format, in the encoding that
 * svc_display_fill() takes: the pixel bytes, least significant first.
 */
static inline uint32_t app_display_color(uint32_t fmt, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t v;

	switch (fmt) {
	case PIXEL_FORMAT_ARGB_8888:
	case PIXEL_FORMAT_XRGB_8888:
		return 0xff000000U | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
	case PIXEL_FORMAT_RGB_888:
		return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
	case PIXEL_FORMAT_BGR_888:
		return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
	case PIXEL_FORMAT_RGB_565:
		return ((uint32_t)(r >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (b >> 3);
	case PIXEL_FORMAT_RGB_565X:
		v = ((uint32_t)(r >> 3) << 11) | ((uint32_t)(g >> 2) << 5) | (b >> 3);
		return ((v & 0xffU) << 8) | (v >> 8);
	case PIXEL_FORMAT_L_8:
		return ((uint32_t)r * 77U + (uint32_t)g * 150U + (uint32_t)b * 29U) >> 8;
	default:
		return 0U;
	}
}

#endif /* APP_DISPLAY_COLOR_H_ */
