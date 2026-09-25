/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A loadable application that sweeps a bar across the display viewport it was
 * granted, above a gradient band, in the tint picked by its argument. Each step
 * is drawn right after a panel refresh when the panel reports one, and the
 * frame rate is shown in a box in the top right corner.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <stdio.h>

#include "app_display_color.h"

#define FRAME_MS    40
/* A tick count needs no conversion from milliseconds at run time. */
#define FRAME_TICKS MAX(FRAME_MS * CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000, 1)
#define BAND_H      24
#define MAX_ROW_PX  1024
#define LOG_EVERY   100

#define DIG_W   12U
#define DIG_H   20U
#define SEG_T   3U
#define BOX_PAD 4U
#define BOX_W   (3U * BOX_PAD + 2U * DIG_W)
#define BOX_H   (2U * BOX_PAD + DIG_H)
#define TOP_H   (BOX_H + 4U)

struct segment {
	uint8_t x;
	uint8_t y;
	uint8_t w;
	uint8_t h;
};

/* Seven-segment layout, segments a to g. */
static const struct segment segments[7] = {
	{ 0U, 0U, DIG_W, SEG_T },
	{ DIG_W - SEG_T, 0U, SEG_T, DIG_H / 2U },
	{ DIG_W - SEG_T, DIG_H / 2U, SEG_T, DIG_H / 2U },
	{ 0U, DIG_H - SEG_T, DIG_W, SEG_T },
	{ 0U, DIG_H / 2U, SEG_T, DIG_H / 2U },
	{ 0U, 0U, SEG_T, DIG_H / 2U },
	{ 0U, DIG_H / 2U - SEG_T / 2U, DIG_W, SEG_T },
};

static const uint8_t digit_segments[10] = {
	0x3fU, 0x06U, 0x5bU, 0x4fU, 0x66U, 0x6dU, 0x7dU, 0x07U, 0x7fU, 0x6fU,
};

static uint8_t band_row[MAX_ROW_PX * sizeof(uint32_t)];
static uint8_t box_buf[BOX_W * BOX_H * sizeof(uint32_t)];

static void put_pixel(uint8_t *dst, uint32_t px, uint8_t bpp)
{
	for (uint8_t i = 0U; i < bpp; i++) {
		dst[i] = (uint8_t)(px >> (8U * i));
	}
}

static bool digit_lit(uint32_t d, uint32_t x, uint32_t y)
{
	for (int i = 0; i < ARRAY_SIZE(segments); i++) {
		if ((digit_segments[d] & BIT(i)) != 0U && x >= segments[i].x &&
		    x < segments[i].x + segments[i].w && y >= segments[i].y &&
		    y < segments[i].y + segments[i].h) {
			return true;
		}
	}
	return false;
}

/* The box is composed in application memory and drawn with one write, so the
 * panel never shows it cleared or half drawn.
 */
static void draw_fps(uint16_t x, uint32_t fps, uint8_t bpp, uint32_t box, uint32_t on)
{
	const uint32_t digits[2] = { MIN(fps, 99U) / 10U, MIN(fps, 99U) % 10U };

	for (uint32_t py = 0U; py < BOX_H; py++) {
		for (uint32_t px = 0U; px < BOX_W; px++) {
			bool lit = false;

			for (uint32_t k = 0U; k < 2U; k++) {
				uint32_t ox = BOX_PAD + k * (DIG_W + BOX_PAD);

				if (px >= ox && py >= BOX_PAD &&
				    digit_lit(digits[k], px - ox, py - BOX_PAD)) {
					lit = true;
				}
			}
			put_pixel(&box_buf[(py * BOX_W + px) * bpp], lit ? on : box, bpp);
		}
	}
	(void)svc_display_write(x, 0U, BOX_W, BOX_H, box_buf, (size_t)BOX_W * BOX_H * bpp);
}

void app_main(void *arg)
{
	int idx = (int)(uintptr_t)arg;
	const uint8_t *tint = app_display_tint[idx & 1];
	struct svc_display_info info;
	bool vsync = true;
	uint32_t bg;
	uint32_t fg;
	uint32_t box;
	uint32_t digit;
	uint32_t fps = 0U;
	uint32_t shown_fps = 0U;
	uint32_t frames = 0U;
	uint32_t t_last;
	uint32_t now;
	uint16_t band_w;
	uint16_t bar_w;
	uint16_t bar_h;
	uint16_t step;
	uint16_t pos = 0U;
	char line[64];
	int failed = 0;
	int rc;

	rc = svc_display_info(&info);
	(void)snprintf(line, sizeof(line), "display: app %d viewport %ux%u rc=%d", idx, info.width,
		       info.height, rc);
	svc_log(line);
	if (rc != 0 || info.width < 2U * BOX_W || info.height <= TOP_H + BAND_H + 16U) {
		return;
	}

	bg = app_display_color(info.pixel_format, 16U, 16U, 24U);
	fg = app_display_color(info.pixel_format, tint[0], tint[1], tint[2]);
	box = app_display_color(info.pixel_format, 40U, 40U, 48U);
	digit = app_display_color(info.pixel_format, 230U, 230U, 230U);
	(void)svc_display_fill(0U, 0U, info.width, info.height, bg);

	band_w = MIN(info.width, MAX_ROW_PX);
	for (uint16_t x = 0U; x < band_w; x++) {
		uint32_t px = app_display_color(info.pixel_format, (uint8_t)(tint[0] * x / band_w),
						(uint8_t)(tint[1] * x / band_w),
						(uint8_t)(tint[2] * x / band_w));

		put_pixel(&band_row[x * info.bytes_per_pixel], px, info.bytes_per_pixel);
	}
	for (uint16_t row = 0U; row < BAND_H; row++) {
		(void)svc_display_write(0U, info.height - BAND_H + row, band_w, 1U, band_row,
					(size_t)band_w * info.bytes_per_pixel);
	}

	bar_w = MAX(info.width / 8U, 8U);
	bar_h = info.height - TOP_H - BAND_H - 4U;
	step = MAX(info.width / 192U, 2U);
	draw_fps(info.width - BOX_W, fps, info.bytes_per_pixel, box, digit);
	(void)svc_display_fill(pos, TOP_H, bar_w, bar_h, fg);
	t_last = svc_uptime_ms();

	for (int frame = 1;; frame++) {
		if (vsync && svc_display_vsync() == -ENOTSUP) {
			vsync = false;
			svc_log("display: no refresh events, pacing by sleep");
		}
		if (!vsync) {
			k_sleep(K_TICKS(FRAME_TICKS));
		}

		if (fps != shown_fps) {
			draw_fps(info.width - BOX_W, fps, info.bytes_per_pixel, box, digit);
			shown_fps = fps;
		}

		if (pos + bar_w + step > info.width) {
			rc = svc_display_fill(pos, TOP_H, bar_w, bar_h, bg);
			pos = 0U;
			if (rc == 0) {
				rc = svc_display_fill(pos, TOP_H, bar_w, bar_h, fg);
			}
		} else {
			rc = svc_display_fill(pos, TOP_H, step, bar_h, bg);
			if (rc == 0) {
				rc = svc_display_fill(pos + bar_w, TOP_H, step, bar_h, fg);
			}
			pos += step;
		}
		if (rc != 0) {
			failed++;
		}

		frames++;
		now = svc_uptime_ms();
		if (now - t_last >= 1000U) {
			fps = (frames * 1000U + (now - t_last) / 2U) / (now - t_last);
			frames = 0U;
			t_last = now;
		}

		if ((frame % LOG_EVERY) == 0) {
			(void)snprintf(line, sizeof(line),
				       "display: app %d frame %d, %d failed, %u fps", idx, frame,
				       failed, fps);
			svc_log(line);
		}
	}
}
EXPORT_SYMBOL(app_main);
