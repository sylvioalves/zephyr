/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* A loadable application that animates the display viewport it was granted, in
 * the tint picked by its argument. Application 0 renders whole frames of moving
 * stripes into its canvas and presents them; application 1 sweeps a bar across
 * its viewport with fills and presents only the parts of its canvas it
 * composes. Each frame is shown right after a panel refresh when the panel
 * reports one, and the frame rate is shown in a box in the top right corner.
 * The module keeps no buffers of its own: everything it composes is in its
 * canvas.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "app_display_color.h"

#define FRAME_MS    40
/* A tick count needs no conversion from milliseconds at run time. */
#define FRAME_TICKS MAX(FRAME_MS * CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000, 1)
#define BAND_H      24
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

struct rate {
	uint32_t fps;
	uint32_t frames;
	uint32_t t_last;
};

struct colors {
	uint32_t bg;
	uint32_t fg;
	uint32_t box;
	uint32_t digit;
};

/* Compose the frame-rate box into @p dst, whose rows are @p pitch pixels apart. */
static void render_box(uint8_t *dst, uint32_t pitch, uint8_t bpp, uint32_t fps,
		       const struct colors *c)
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
			put_pixel(&dst[(py * pitch + px) * bpp], lit ? c->digit : c->box, bpp);
		}
	}
}

/* Wait for the next frame: a panel refresh, or a sleep when none is reported. */
static void pace(bool *vsync)
{
	if (*vsync && svc_display_vsync() == -ENOTSUP) {
		*vsync = false;
		svc_log("display: no refresh events, pacing by sleep");
	}
	if (!*vsync) {
		k_sleep(K_TICKS(FRAME_TICKS));
	}
}

static void rate_update(struct rate *r)
{
	uint32_t now = svc_uptime_ms();

	r->frames++;
	if (now - r->t_last >= 1000U) {
		r->fps = (r->frames * 1000U + (now - r->t_last) / 2U) / (now - r->t_last);
		r->frames = 0U;
		r->t_last = now;
	}
}

static uint32_t stripe_color(uint32_t fmt, const uint8_t *tint, uint32_t x, uint32_t w)
{
	uint32_t level = (x < w / 2U ? x : w - x) * 510U / w;

	return app_display_color(fmt, (uint8_t)(tint[0] * level / 255U),
				 (uint8_t)(tint[1] * level / 255U),
				 (uint8_t)(tint[2] * level / 255U));
}

/* The bar is drawn with fills; with a canvas the gradient band and the
 * frame-rate box are composed in it and presented as whole rectangles, so the
 * panel never shows them cleared or half drawn.
 */
static void run_bars(int idx, const struct svc_display_info *info,
		     const struct svc_display_canvas *cv, const uint8_t *tint,
		     const struct colors *c)
{
	uint8_t bpp = info->bytes_per_pixel;
	uint8_t *base = cv != NULL ? (uint8_t *)cv->base : NULL;
	uint32_t pitch = cv != NULL ? cv->pitch : 0U;
	struct rate r = { .t_last = svc_uptime_ms() };
	uint32_t shown_fps = UINT32_MAX;
	bool vsync = true;
	uint16_t bar_w = MAX(info->width / 8U, 8U);
	uint16_t bar_h = info->height - TOP_H - BAND_H - 4U;
	uint16_t step = MAX(info->width / 192U, 2U);
	uint16_t pos = 0U;
	char line[64];
	int failed = 0;
	int rc;

	(void)svc_display_fill(0U, 0U, info->width, info->height, c->bg);
	if (base != NULL) {
		for (uint32_t y = info->height - BAND_H; y < info->height; y++) {
			for (uint32_t x = 0U; x < info->width; x++) {
				uint32_t px = app_display_color(
					info->pixel_format, (uint8_t)(tint[0] * x / info->width),
					(uint8_t)(tint[1] * x / info->width),
					(uint8_t)(tint[2] * x / info->width));

				put_pixel(&base[(y * pitch + x) * bpp], px, bpp);
			}
		}
		(void)svc_display_present(0U, info->height - BAND_H, info->width, BAND_H);
	}
	(void)svc_display_fill(pos, TOP_H, bar_w, bar_h, c->fg);

	for (int frame = 1;; frame++) {
		pace(&vsync);

		if (base != NULL && r.fps != shown_fps) {
			render_box(&base[(info->width - BOX_W) * bpp], pitch, bpp, r.fps, c);
			(void)svc_display_present(info->width - BOX_W, 0U, BOX_W, BOX_H);
			shown_fps = r.fps;
		}

		if (pos + bar_w + step > info->width) {
			rc = svc_display_fill(pos, TOP_H, bar_w, bar_h, c->bg);
			pos = 0U;
			if (rc == 0) {
				rc = svc_display_fill(pos, TOP_H, bar_w, bar_h, c->fg);
			}
		} else {
			rc = svc_display_fill(pos, TOP_H, step, bar_h, c->bg);
			if (rc == 0) {
				rc = svc_display_fill(pos + bar_w, TOP_H, step, bar_h, c->fg);
			}
			pos += step;
		}
		if (rc != 0) {
			failed++;
		}

		rate_update(&r);
		if ((frame % LOG_EVERY) == 0) {
			(void)snprintf(line, sizeof(line),
				       "display: app %d frame %d, %d failed, %u fps", idx, frame,
				       failed, r.fps);
			svc_log(line);
		}
	}
}

/* Each row repeats the one above it shifted right by one pixel, so only the
 * first row of the stripes is computed per frame and they move diagonally.
 */
static void run_canvas(int idx, const struct svc_display_canvas *cv, const uint8_t *tint,
		       const struct colors *c)
{
	uint8_t bpp = cv->bytes_per_pixel;
	uint8_t *base = (uint8_t *)cv->base;
	uint32_t row_bytes = (uint32_t)cv->pitch * bpp;
	uint32_t w = cv->width;
	uint32_t step = MAX(w / 192U, 2U);
	struct rate r = { .t_last = svc_uptime_ms() };
	uint32_t shown_fps = UINT32_MAX;
	uint32_t present_ms = 0U;
	uint32_t render_ms = 0U;
	uint32_t phase = 0U;
	uint32_t t0;
	bool vsync = true;
	char line[64];
	int failed = 0;

	for (uint32_t y = 0U; y < TOP_H; y++) {
		for (uint32_t x = 0U; x < w; x++) {
			put_pixel(&base[y * row_bytes + x * bpp], c->bg, bpp);
		}
	}

	for (int frame = 1;; frame++) {
		uint8_t *first = &base[TOP_H * row_bytes];

		t0 = svc_uptime_ms();
		if (r.fps != shown_fps) {
			render_box(&base[(w - BOX_W) * bpp], cv->pitch, bpp, r.fps, c);
			shown_fps = r.fps;
		}
		for (uint32_t x = 0U; x < w; x++) {
			uint32_t px = stripe_color(cv->pixel_format, tint, (x + w - phase) % w, w);

			put_pixel(&first[x * bpp], px, bpp);
		}
		for (uint32_t y = TOP_H + 1U; y < cv->height; y++) {
			uint8_t *dst = &base[y * row_bytes];
			const uint8_t *src = dst - row_bytes;

			memcpy(&dst[bpp], src, (w - 1U) * bpp);
			memcpy(dst, &src[(w - 1U) * bpp], bpp);
		}
		phase = (phase + step) % w;
		render_ms += svc_uptime_ms() - t0;

		pace(&vsync);
		t0 = svc_uptime_ms();
		if (svc_display_present(0U, 0U, w, cv->height) != 0) {
			failed++;
		}
		present_ms += svc_uptime_ms() - t0;

		rate_update(&r);
		if ((frame % LOG_EVERY) == 0) {
			(void)snprintf(line, sizeof(line),
				       "display: app %d frame %d, %d failed, %u fps", idx, frame,
				       failed, r.fps);
			svc_log(line);
			(void)snprintf(line, sizeof(line),
				       "display: app %d render %u.%02u ms, present %u.%02u ms", idx,
				       render_ms / LOG_EVERY, render_ms % LOG_EVERY,
				       present_ms / LOG_EVERY, present_ms % LOG_EVERY);
			svc_log(line);
			render_ms = 0U;
			present_ms = 0U;
		}
	}
}

void app_main(void *arg)
{
	int idx = (int)(uintptr_t)arg;
	const uint8_t *tint = app_display_tint[idx & 1];
	struct svc_display_canvas cv;
	struct svc_display_info info;
	struct colors c;
	char line[64];
	int rc;

	rc = svc_display_info(&info);
	(void)snprintf(line, sizeof(line), "display: app %d viewport %ux%u rc=%d", idx, info.width,
		       info.height, rc);
	svc_log(line);
	if (rc != 0 || info.width < 2U * BOX_W || info.height <= TOP_H + BAND_H + 16U) {
		return;
	}

	c.bg = app_display_color(info.pixel_format, 16U, 16U, 24U);
	c.fg = app_display_color(info.pixel_format, tint[0], tint[1], tint[2]);
	c.box = app_display_color(info.pixel_format, 40U, 40U, 48U);
	c.digit = app_display_color(info.pixel_format, 230U, 230U, 230U);

	rc = svc_display_canvas(&cv);
	(void)snprintf(line, sizeof(line), "display: app %d canvas rc=%d", idx, rc);
	svc_log(line);
	if (rc == 0 && idx == 0) {
		run_canvas(idx, &cv, tint, &c);
	}
	run_bars(idx, &info, rc == 0 ? &cv : NULL, tint, &c);
}
EXPORT_SYMBOL(app_main);
