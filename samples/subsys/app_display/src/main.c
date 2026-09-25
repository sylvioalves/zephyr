/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Share one display between the supervisor and two verified applications. The
 * supervisor draws a status bar with one tile per application across the top,
 * and each application animates its own half of the panel below it. After a
 * while one application is stopped: its tile turns red and the platform clears
 * its half, while the other keeps drawing.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "app_display_color.h"

#define PANEL_NODE    DT_CHOSEN(zephyr_display)
#define PANEL_W       DT_PROP(PANEL_NODE, width)
#define BAR_H         32U
#define GAP           4U
#define NUM_APPS      2
#define STOP_AFTER_MS 6000
#define WATCH_MS      3000

static const struct device *const panel = DEVICE_DT_GET(PANEL_NODE);

static const uint8_t display_app[] = {
#include "display_app.inc"
};
static const uint8_t display_app_hash[32] = {
#include "display_app.sha256.inc"
};

static const char *const app_names[NUM_APPS] = { "left", "right" };

static uint8_t row_buf[PANEL_W * sizeof(uint32_t)];
static struct display_capabilities caps;
static uint8_t bpp;

static void panel_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color)
{
	struct display_buffer_descriptor desc = {
		.buf_size = (uint32_t)w * bpp,
		.width = w,
		.height = 1U,
		.pitch = w,
	};

	for (uint16_t i = 0U; i < w; i++) {
		for (uint8_t b = 0U; b < bpp; b++) {
			row_buf[i * bpp + b] = (uint8_t)(color >> (8U * b));
		}
	}

	(void)app_platform_display_lock(K_FOREVER);
	for (uint16_t r = 0U; r < h; r++) {
		desc.frame_incomplete = (r + 1U < h);
		(void)display_write(panel, x, y + r, &desc, row_buf);
	}
	app_platform_display_unlock();
}

static void draw_status_bar(const bool running[NUM_APPS])
{
	uint32_t fmt = caps.current_pixel_format;
	uint16_t tile_w = caps.x_resolution / 8U;

	panel_fill(0U, 0U, caps.x_resolution, BAR_H, app_display_color(fmt, 48U, 48U, 56U));
	for (int i = 0; i < NUM_APPS; i++) {
		const uint8_t *t = app_display_tint[i];
		uint32_t color = running[i] ? app_display_color(fmt, t[0], t[1], t[2])
					    : app_display_color(fmt, 200U, 0U, 0U);

		panel_fill(GAP + i * (tile_w + GAP), GAP, tile_w, BAR_H - 2U * GAP, color);
	}
}

static int launch_app(int i, const struct app_display_rect *vp)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	int rc;

	rc = app_loader_verify_load(app_names[i], display_app, ARRAY_SIZE(display_app),
				    display_app_hash, "app_main", &ext, &entry);
	if (rc != 0) {
		printk("%s: load failed: %d\n", app_names[i], rc);
		return rc;
	}

	/* Slots are taken in order on a fresh boot, so application i gets slot i
	 * and its viewport is granted before it runs.
	 */
	rc = app_platform_grant_display(i, vp);
	if (rc != 0) {
		printk("%s: grant failed: %d\n", app_names[i], rc);
		return rc;
	}

	return app_platform_launch_llext(app_names[i], ext, (app_entry_t)entry,
					 (void *)(uintptr_t)i,
					 APP_CAP_LOG | APP_CAP_DISPLAY | APP_CAP_TIME);
}

int main(void)
{
	bool running[NUM_APPS] = { true, true };
	struct app_platform_status st;
	struct app_display_rect vp[NUM_APPS];
	int id[NUM_APPS];
	uint16_t half;
	uint16_t top;
	int rc;

	printk("\n*** app_display: a shared display through a platform service ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	if (!device_is_ready(panel)) {
		printk("display not ready\n");
		return 0;
	}

	display_get_capabilities(panel, &caps);
	bpp = DISPLAY_BITS_PER_PIXEL(caps.current_pixel_format) / 8U;
	printk("app_display: panel %ux%u, %u byte(s) per pixel\n", caps.x_resolution,
	       caps.y_resolution, bpp);
	if (bpp == 0U || caps.x_resolution > PANEL_W) {
		printk("unsupported panel format\n");
		return 0;
	}

	(void)app_platform_display_lock(K_FOREVER);
	(void)display_blanking_off(panel);
	app_platform_display_unlock();
	draw_status_bar(running);

	half = caps.x_resolution / 2U;
	top = BAR_H + GAP;
	vp[0] = (struct app_display_rect){
		.x = 0U, .y = top, .width = half - GAP / 2U, .height = caps.y_resolution - top,
	};
	vp[1] = (struct app_display_rect){
		.x = half + GAP / 2U, .y = top, .width = half - GAP / 2U,
		.height = caps.y_resolution - top,
	};

	for (int i = 0; i < NUM_APPS; i++) {
		id[i] = launch_app(i, &vp[i]);
		if (id[i] != i) {
			printk("%s: launch failed: %d\n", app_names[i], id[i]);
			return 0;
		}
	}

	printk("app_display: two apps drawing, stopping '%s' in %d ms\n", app_names[1],
	       STOP_AFTER_MS);
	k_msleep(STOP_AFTER_MS);

	rc = app_platform_stop(id[1]);
	printk("app_display: stopped '%s' rc=%d\n", app_names[1], rc);
	running[1] = false;
	draw_status_bar(running);

	k_msleep(WATCH_MS);
	rc = app_platform_status(id[0], &st);
	printk("\n*** done: '%s' active=%d after '%s' was stopped ***\n", app_names[0],
	       rc == 0 && st.active, app_names[1]);
	return 0;
}
