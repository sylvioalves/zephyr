/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App launcher: a touch/keypad LVGL UI that lists isolated applications as
 * tiles and lets the user start or kill each one, with live status. The UI
 * talks only to launcher.h, so it runs unchanged on the host simulator and on
 * memory-protected hardware where the tiles control real isolated apps.
 */

#include "launcher.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>

struct tile {
	lv_obj_t *panel;
	lv_obj_t *name_lbl;
	lv_obj_t *state_lbl;
	lv_obj_t *btn_lbl;
	int       idx;
};

static struct tile tiles[LAUNCHER_MAX_APPS];

static const char *state_text(enum launcher_app_state s)
{
	switch (s) {
	case LAUNCHER_RUNNING: return "RUNNING";
	case LAUNCHER_CRASHED: return "CRASHED";
	default:               return "stopped";
	}
}

static uint32_t state_color(enum launcher_app_state s)
{
	switch (s) {
	case LAUNCHER_RUNNING: return 0x2e7d32;  /* green  */
	case LAUNCHER_CRASHED: return 0xc62828;  /* red    */
	default:               return 0x555555;  /* grey   */
	}
}

/* Toggle: start a stopped/crashed app, kill a running one. */
static void tile_btn_cb(lv_event_t *e)
{
	struct tile *t = lv_event_get_user_data(e);
	struct launcher_app_info info;

	if (launcher_app_get(t->idx, &info) != 0) {
		return;
	}
	if (info.state == LAUNCHER_RUNNING) {
		launcher_app_kill(t->idx);
	} else {
		launcher_app_start(t->idx);
	}
}

static void build_ui(void)
{
	lv_obj_t *scr = lv_scr_act();

	lv_obj_t *title = lv_label_create(scr);

	lv_label_set_text(title, "App Launcher");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

	int n = launcher_app_count();

	for (int i = 0; i < n && i < LAUNCHER_MAX_APPS; i++) {
		struct launcher_app_info info;

		launcher_app_get(i, &info);

		lv_obj_t *panel = lv_obj_create(scr);

		lv_obj_set_size(panel, LV_PCT(94), 52);
		lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 26 + i * 56);
		lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
				      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *name_lbl = lv_label_create(panel);

		lv_label_set_text(name_lbl, info.name);

		lv_obj_t *state_lbl = lv_label_create(panel);

		lv_label_set_text(state_lbl, state_text(info.state));

		lv_obj_t *btn = lv_button_create(panel);

		lv_obj_set_size(btn, 70, 34);

		lv_obj_t *btn_lbl = lv_label_create(btn);

		lv_label_set_text(btn_lbl, "Start");
		lv_obj_center(btn_lbl);

		tiles[i].panel = panel;
		tiles[i].name_lbl = name_lbl;
		tiles[i].state_lbl = state_lbl;
		tiles[i].btn_lbl = btn_lbl;
		tiles[i].idx = i;

		lv_obj_add_event_cb(btn, tile_btn_cb, LV_EVENT_CLICKED, &tiles[i]);
	}
}

static void refresh_ui(void)
{
	int n = launcher_app_count();

	for (int i = 0; i < n && i < LAUNCHER_MAX_APPS; i++) {
		struct launcher_app_info info;

		if (launcher_app_get(i, &info) != 0) {
			continue;
		}

		static char buf[24];

		if (info.state == LAUNCHER_RUNNING) {
			snprintf(buf, sizeof(buf), "RUNNING  %u", info.ticks);
		} else {
			snprintf(buf, sizeof(buf), "%s", state_text(info.state));
		}
		lv_label_set_text(tiles[i].state_lbl, buf);
		lv_obj_set_style_text_color(tiles[i].state_lbl,
					    lv_color_hex(state_color(info.state)), 0);
		lv_label_set_text(tiles[i].btn_lbl,
				  info.state == LAUNCHER_RUNNING ? "Kill" : "Start");
	}
}

int main(void)
{
	const struct device *display_dev =
		DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display_dev)) {
		return 0;
	}

	launcher_init();
	build_ui();
	lv_timer_handler();
	display_blanking_off(display_dev);

	/*
	 * Headless self-exercise so the lifecycle is observable without a screen:
	 * start two apps, including the buggy one, and let it crash + be
	 * reclaimed. On hardware with a panel, the user drives this by touch.
	 */
	launcher_app_start(0);
	launcher_app_start(2);   /* the buggy app */

	int prev[LAUNCHER_MAX_APPS] = { -1, -1, -1, -1 };

	while (1) {
		int n = launcher_app_count();

		for (int i = 0; i < n && i < LAUNCHER_MAX_APPS; i++) {
			struct launcher_app_info info;

			launcher_app_get(i, &info);
			if ((int)info.state != prev[i]) {
				printk("launcher: app '%s' -> %s\n", info.name,
				       info.state == LAUNCHER_RUNNING ? "RUNNING" :
				       info.state == LAUNCHER_CRASHED ? "CRASHED" :
				       "stopped");
				prev[i] = info.state;
			}
		}

		refresh_ui();
		uint32_t sleep = lv_timer_handler();

		k_msleep(MIN(sleep, 100));
	}
	return 0;
}
