/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Addressable RGB LED service. The platform owns the strip on the led-strip
 * alias and keeps its colors; an application sets only the pixels it was
 * granted, and every change refreshes the whole strip.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include "app_grants.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>
#include <string.h>

#define RGB_NODE  DT_ALIAS(led_strip)
#define RGB_COUNT MIN(DT_PROP(RGB_NODE, chain_length), 32)

static const struct device *const strip = DEVICE_DT_GET(RGB_NODE);

static struct led_rgb pixels[RGB_COUNT];
/* The driver may overwrite the buffer it is handed, so it gets a copy. */
static struct led_rgb frame[RGB_COUNT];
/* Bit N set means an application may set pixel N. */
static uint32_t rgb_grant[MAX_APPS];
/* Bit N set means this application last set pixel N to a non-black color. */
static uint32_t rgb_lit[MAX_APPS];
static SVC_LOCK_DEFINE(rgb_lock);

static int rgb_refresh(void)
{
	memcpy(frame, pixels, sizeof(frame));
	return led_strip_update_rgb(strip, frame, RGB_COUNT);
}

void app_platform_grant_rgb(int id, uint32_t pixel_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		rgb_grant[id] = pixel_mask;
	}
}

int z_impl_app_svc_rgb_set(int pixel, uint8_t r, uint8_t g, uint8_t b)
{
	if (!app_has_cap(APP_CAP_RGB)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (pixel < 0 || pixel >= (int)RGB_COUNT || id < 0 || id >= MAX_APPS ||
	    (rgb_grant[id] & BIT(pixel)) == 0U) {
		return -EACCES;
	}
	if (!device_is_ready(strip)) {
		return -ENODEV;
	}
	if (app_service_lock(&rgb_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}

	struct led_rgb prev = pixels[pixel];

	pixels[pixel] = (struct led_rgb){ .r = r, .g = g, .b = b };

	int rc = rgb_refresh();

	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			rgb_lit[i] &= ~BIT(pixel);
		}
		if (r != 0U || g != 0U || b != 0U) {
			rgb_lit[id] |= BIT(pixel);
		}
	} else {
		pixels[pixel] = prev;
	}
	(void)app_service_unlock(&rgb_lock);
	return rc;
}

int z_impl_app_svc_rgb_count(void)
{
	return (int)RGB_COUNT;
}

void app_platform_clear_rgb_grants(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	rgb_grant[id] = 0U;
	if (app_service_lock(&rgb_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
	if (rgb_lit[id] != 0U) {
		for (int i = 0; i < (int)RGB_COUNT; i++) {
			if ((rgb_lit[id] & BIT(i)) != 0U) {
				pixels[i] = (struct led_rgb){ 0 };
			}
		}
		if (device_is_ready(strip)) {
			(void)rgb_refresh();
		}
	}
	rgb_lit[id] = 0U;
	(void)app_service_unlock(&rgb_lock);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_rgb_set(int pixel, uint8_t r, uint8_t g, uint8_t b)
{
	return z_impl_app_svc_rgb_set(pixel, r, g, b);
}
#include <zephyr/syscalls/app_svc_rgb_set_mrsh.c>

static inline int z_vrfy_app_svc_rgb_count(void)
{
	return z_impl_app_svc_rgb_count();
}
#include <zephyr/syscalls/app_svc_rgb_count_mrsh.c>
#endif /* CONFIG_USERSPACE */
