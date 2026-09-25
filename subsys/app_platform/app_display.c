/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Shared display service. The platform owns the zephyr,display panel; an
 * application draws only inside the viewport it was granted. Panel drivers are
 * not reentrant, so every panel access goes through one lock, and application
 * pixels are staged in supervisor memory before that lock is taken.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include "app_grants.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/display.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>
#include <string.h>

#define XFER_MAX CONFIG_APP_PLATFORM_DISPLAY_XFER_MAX

#define VSYNC_EVENT   BIT(0)
#define VSYNC_TIMEOUT K_MSEC(100)

static const struct device *const panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

struct display_slot {
	struct app_display_rect vp;
	bool drew;
	struct k_mutex stage_lock;
};

static struct display_slot slots[MAX_APPS];
static uint8_t stage[MAX_APPS][XFER_MAX] __aligned(4);
static struct k_spinlock vp_lock;
static K_MUTEX_DEFINE(display_lock);
static K_EVENT_DEFINE(vsync_event);
static bool vsync_ok;

static int display_slots_init(void)
{
	for (int i = 0; i < MAX_APPS; i++) {
		k_mutex_init(&slots[i].stage_lock);
	}
	return 0;
}
SYS_INIT(display_slots_init, PRE_KERNEL_1, 0);

static enum display_event_result on_vsync(const struct device *dev, uint32_t evt,
					  const struct display_event_data *data, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(evt);
	ARG_UNUSED(data);
	ARG_UNUSED(user_data);

	k_event_post(&vsync_event, VSYNC_EVENT);
	return DISPLAY_EVENT_RESULT_CONTINUE;
}

static int display_vsync_init(void)
{
	uint32_t handle;

	vsync_ok = device_is_ready(panel) &&
		   display_register_event_cb(panel, on_vsync, NULL, DISPLAY_EVENT_VSYNC, true,
					     &handle) == 0;
	return 0;
}
SYS_INIT(display_vsync_init, APPLICATION, 0);

static int panel_caps(struct display_capabilities *caps, uint8_t *bpp)
{
	uint32_t bits;

	if (!device_is_ready(panel)) {
		return -ENODEV;
	}
	display_get_capabilities(panel, caps);
	bits = DISPLAY_BITS_PER_PIXEL(caps->current_pixel_format);
	if (bits == 0U || (bits % 8U) != 0U) {
		return -ENOTSUP;
	}
	*bpp = (uint8_t)(bits / 8U);
	return 0;
}

static struct app_display_rect viewport_of(int id)
{
	k_spinlock_key_t key = k_spin_lock(&vp_lock);
	struct app_display_rect vp = slots[id].vp;

	k_spin_unlock(&vp_lock, key);
	return vp;
}

static int display_access(int *id, struct app_display_rect *vp, struct display_capabilities *caps,
			  uint8_t *bpp)
{
	if (!app_has_cap(APP_CAP_DISPLAY)) {
		return -EPERM;
	}

	*id = app_platform_id_current();
	if (*id < 0 || *id >= MAX_APPS) {
		return -EACCES;
	}
	*vp = viewport_of(*id);
	if (vp->width == 0U || vp->height == 0U) {
		return -EACCES;
	}
	return panel_caps(caps, bpp);
}

static int check_rect(const struct app_display_rect *vp, uint16_t x, uint16_t y, uint16_t w,
		      uint16_t h)
{
	if (w == 0U || h == 0U || (uint32_t)x + w > vp->width || (uint32_t)y + h > vp->height) {
		return -ERANGE;
	}
	return 0;
}

/* Write a viewport rectangle through the slot's staging buffer, in chunks of
 * whole rows, or of part of one row when a row does not fit. With @p src NULL
 * the buffer already holds a run of one color and is written as it is.
 */
static int blit(int id, const struct app_display_rect *vp, uint16_t x, uint16_t y, uint16_t w,
		uint16_t h, const uint8_t *src, uint8_t bpp)
{
	uint8_t *buf = stage[id];
	uint32_t seg = MIN((uint32_t)w, XFER_MAX / bpp);
	uint32_t rows_max = (seg == w) ? XFER_MAX / (seg * bpp) : 1U;
	uint32_t row_bytes = (uint32_t)w * bpp;
	uint32_t rows;
	int rc;

	for (uint32_t r = 0U; r < h; r += rows) {
		rows = MIN(rows_max, (uint32_t)h - r);
		for (uint32_t c = 0U; c < w; c += seg) {
			uint32_t cw = MIN(seg, (uint32_t)w - c);
			struct display_buffer_descriptor desc = {
				.buf_size = cw * rows * bpp,
				.width = (uint16_t)cw,
				.height = (uint16_t)rows,
				.pitch = (uint16_t)cw,
				.frame_incomplete = (r + rows < h) || (c + cw < w),
			};

			if (src != NULL) {
				for (uint32_t i = 0U; i < rows; i++) {
					memcpy(&buf[i * cw * bpp],
					       &src[(r + i) * row_bytes + c * bpp], cw * bpp);
				}
			}
			if (app_svc_lock(&display_lock, SVC_LOCK_TIMEOUT) != 0) {
				return -EBUSY;
			}
			slots[id].drew = true;
			rc = display_write(panel, vp->x + x + c, vp->y + y + r, &desc, buf);
			(void)app_svc_unlock(&display_lock);
			if (rc != 0) {
				return -EIO;
			}
		}
	}
	return 0;
}

static int fill(int id, const struct app_display_rect *vp, uint16_t x, uint16_t y, uint16_t w,
		uint16_t h, uint32_t color, uint8_t bpp)
{
	uint8_t px[sizeof(uint32_t)];

	sys_put_le32(color, px);
	for (uint32_t i = 0U; i + bpp <= XFER_MAX; i += bpp) {
		memcpy(&stage[id][i], px, bpp);
	}
	return blit(id, vp, x, y, w, h, NULL, bpp);
}

int app_platform_grant_display(int id, const struct app_display_rect *vp)
{
	struct app_display_rect set = { 0 };
	k_spinlock_key_t key;

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	if (vp != NULL && vp->width != 0U && vp->height != 0U) {
		struct display_capabilities caps;

		if (!device_is_ready(panel)) {
			return -ENODEV;
		}
		display_get_capabilities(panel, &caps);
		if ((uint32_t)vp->x + vp->width > caps.x_resolution ||
		    (uint32_t)vp->y + vp->height > caps.y_resolution) {
			return -EINVAL;
		}
		set = *vp;
	}

	key = k_spin_lock(&vp_lock);
	slots[id].vp = set;
	k_spin_unlock(&vp_lock, key);
	return 0;
}

int app_platform_display_lock(k_timeout_t timeout)
{
	return k_mutex_lock(&display_lock, timeout) == 0 ? 0 : -EBUSY;
}

void app_platform_display_unlock(void)
{
	(void)k_mutex_unlock(&display_lock);
}

int z_impl_svc_display_info(struct svc_display_info *out)
{
	struct display_capabilities caps;
	struct app_display_rect vp;
	uint8_t bpp;
	int id;
	int rc = display_access(&id, &vp, &caps, &bpp);

	if (rc != 0) {
		return rc;
	}
	out->width = vp.width;
	out->height = vp.height;
	out->pixel_format = caps.current_pixel_format;
	out->bytes_per_pixel = bpp;
	return 0;
}

int z_impl_svc_display_vsync(void)
{
	struct display_capabilities caps;
	struct app_display_rect vp;
	uint8_t bpp;
	int id;
	int rc = display_access(&id, &vp, &caps, &bpp);

	if (rc != 0) {
		return rc;
	}
	if (!vsync_ok) {
		return -ENOTSUP;
	}
	return k_event_wait(&vsync_event, VSYNC_EVENT, true, VSYNC_TIMEOUT) != 0U ? 0 : -EAGAIN;
}

int z_impl_svc_display_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *buf,
			     size_t len)
{
	struct display_capabilities caps;
	struct app_display_rect vp;
	uint8_t bpp;
	int id;
	int rc = display_access(&id, &vp, &caps, &bpp);

	if (rc != 0) {
		return rc;
	}
	rc = check_rect(&vp, x, y, w, h);
	if (rc != 0) {
		return rc;
	}
	if ((size_t)w * h * bpp != len) {
		return -EINVAL;
	}

	(void)k_mutex_lock(&slots[id].stage_lock, K_FOREVER);
	rc = blit(id, &vp, x, y, w, h, buf, bpp);
	(void)k_mutex_unlock(&slots[id].stage_lock);
	return rc;
}

int z_impl_svc_display_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color)
{
	struct display_capabilities caps;
	struct app_display_rect vp;
	uint8_t bpp;
	int id;
	int rc = display_access(&id, &vp, &caps, &bpp);

	if (rc != 0) {
		return rc;
	}
	rc = check_rect(&vp, x, y, w, h);
	if (rc != 0) {
		return rc;
	}

	(void)k_mutex_lock(&slots[id].stage_lock, K_FOREVER);
	rc = fill(id, &vp, x, y, w, h, color, bpp);
	(void)k_mutex_unlock(&slots[id].stage_lock);
	return rc;
}

void app_platform_clear_display_grants(int id)
{
	struct display_capabilities caps;
	struct app_display_rect vp;
	k_spinlock_key_t key;
	uint8_t bpp;

	if (id < 0 || id >= MAX_APPS) {
		return;
	}

	key = k_spin_lock(&vp_lock);
	vp = slots[id].vp;
	slots[id].vp = (struct app_display_rect){ 0 };
	k_spin_unlock(&vp_lock, key);

	k_mutex_init(&slots[id].stage_lock);
	if (slots[id].drew && vp.width != 0U && vp.height != 0U && panel_caps(&caps, &bpp) == 0) {
		(void)fill(id, &vp, 0U, 0U, vp.width, vp.height,
			   CONFIG_APP_PLATFORM_DISPLAY_CLEAR_COLOR, bpp);
	}
	slots[id].drew = false;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_display_info(struct svc_display_info *out)
{
	struct svc_display_info tmp;
	int rc = z_impl_svc_display_info(&tmp);

	if (rc == 0 && k_usermode_to_copy(out, &tmp, sizeof(tmp)) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/svc_display_info_mrsh.c>

static inline int z_vrfy_svc_display_vsync(void)
{
	return z_impl_svc_display_vsync();
}
#include <zephyr/syscalls/svc_display_vsync_mrsh.c>

static inline int z_vrfy_svc_display_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
					   const void *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_display_write(x, y, w, h, buf, len);
}
#include <zephyr/syscalls/svc_display_write_mrsh.c>

static inline int z_vrfy_svc_display_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
					  uint32_t color)
{
	return z_impl_svc_display_fill(x, y, w, h, color);
}
#include <zephyr/syscalls/svc_display_fill_mrsh.c>
#endif /* CONFIG_USERSPACE */
