/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The shared CAN service. The platform owns the controller, starts it at the
 * bitrate its devicetree node gives and runs it on the bus, or in loopback when
 * CONFIG_APP_PLATFORM_CAN_LOOPBACK is set. An application holding APP_CAP_CAN
 * sends and receives frames through copied buffers and never holds the bus.
 *
 * Every received frame is matched in the controller's callback against each
 * application's filters and copied into that application's own queue, so one
 * application never consumes another's frames. An application that installed
 * no filter receives every frame.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/drivers/can.h>
#include <string.h>
#include <errno.h>
#include "app_events.h"
#include "app_grants.h"

#define CAN_PAYLOAD_MAX 8
#define CAN_FILTERS     CONFIG_APP_PLATFORM_CAN_FILTERS
#define CAN_RX_DEPTH    CONFIG_APP_PLATFORM_CAN_RX_DEPTH

#if DT_HAS_CHOSEN(zephyr_canbus)
struct can_app_filter {
	uint32_t id;
	uint32_t mask;
	uint8_t  flags;
	bool     used;
};

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static bool can_ready;
static SVC_LOCK_DEFINE(can_lock);

static struct k_msgq can_rx_q[MAX_APPS];
static char __aligned(4) can_rx_buf[MAX_APPS][CAN_RX_DEPTH * sizeof(struct can_frame)];
static struct can_app_filter can_filters[MAX_APPS][CAN_FILTERS];
static atomic_t can_users;
static uint32_t can_rx_dropped[MAX_APPS];
static int32_t can_tx_error[MAX_APPS];

static bool filter_matches(const struct can_app_filter *f, const struct can_frame *frame)
{
	if (((frame->flags ^ f->flags) & CAN_FRAME_IDE) != 0U) {
		return false;
	}
	if ((f->flags & CAN_FRAME_RTR) != 0U && (frame->flags & CAN_FRAME_RTR) == 0U) {
		return false;
	}
	return ((frame->id ^ f->id) & f->mask) == 0U;
}

static bool app_wants(int id, const struct can_frame *frame)
{
	bool any = false;

	for (int i = 0; i < CAN_FILTERS; i++) {
		if (!can_filters[id][i].used) {
			continue;
		}
		any = true;
		if (filter_matches(&can_filters[id][i], frame)) {
			return true;
		}
	}
	return !any;
}

static void can_rx(const struct device *dev, struct can_frame *frame, void *arg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(arg);

	for (int id = 0; id < MAX_APPS; id++) {
		if (!atomic_test_bit(&can_users, id) || !app_wants(id, frame)) {
			continue;
		}
		if (k_msgq_put(&can_rx_q[id], frame, K_NO_WAIT) != 0) {
			can_rx_dropped[id]++;
		} else {
			app_events_notify(id, APP_EVENT_CAN, 0U);
		}
	}
}

static void can_tx_done(const struct device *dev, int error, void *arg)
{
	ARG_UNUSED(dev);

	int id = (int)(intptr_t)arg;

	if (id >= 0 && id < MAX_APPS && error != 0) {
		can_tx_error[id] = error;
	}
}

static int can_init_queues(void)
{
	for (int i = 0; i < MAX_APPS; i++) {
		k_msgq_init(&can_rx_q[i], can_rx_buf[i], sizeof(struct can_frame), CAN_RX_DEPTH);
	}
	return 0;
}
SYS_INIT(can_init_queues, POST_KERNEL, 0);

static int can_ensure_up(void)
{
	if (can_ready) {
		return 0;
	}
	if (app_service_lock(&can_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}

	int rc = 0;
	can_mode_t mode = IS_ENABLED(CONFIG_APP_PLATFORM_CAN_LOOPBACK) ?
			  CAN_MODE_LOOPBACK : CAN_MODE_NORMAL;
	const struct can_filter std = { .id = 0, .mask = 0, .flags = 0 };
	const struct can_filter ext = { .id = 0, .mask = 0, .flags = CAN_FILTER_IDE };

	if (!can_ready) {
		if (!device_is_ready(can_dev)) {
			rc = -ENODEV;
			goto out;
		}
		if (can_set_mode(can_dev, mode) != 0 || can_start(can_dev) != 0) {
			rc = -EIO;
			goto out;
		}
		if (can_add_rx_filter(can_dev, can_rx, NULL, &std) < 0 ||
		    can_add_rx_filter(can_dev, can_rx, NULL, &ext) < 0) {
			rc = -EIO;
			goto out;
		}
		can_ready = true;
	}
out:
	app_service_unlock(&can_lock);
	return rc;
}

/* Bring the controller up and register the caller as a receiver. */
static int can_join(int *id_out)
{
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	if (can_ensure_up() != 0) {
		return -ENODEV;
	}
	atomic_set_bit(&can_users, id);
	*id_out = id;
	return 0;
}

static int can_send_one(const struct app_can_frame *in)
{
	int id;
	int rc = can_join(&id);

	if (rc != 0) {
		return rc;
	}

	struct can_frame frame = {
		.id = in->id,
		.dlc = in->dlc,
	};

	if ((in->flags & APP_CAN_FRAME_EXT) != 0U) {
		frame.flags |= CAN_FRAME_IDE;
	}
	if ((in->flags & APP_CAN_FRAME_RTR) != 0U) {
		frame.flags |= CAN_FRAME_RTR;
	} else {
		memcpy(frame.data, in->data, in->dlc);
	}
	/* Non-blocking: a NULL callback would wait for the bus to acknowledge. */
	return can_send(can_dev, &frame, K_MSEC(100), can_tx_done, (void *)(intptr_t)id);
}

static int can_recv_one(struct app_can_frame *out, k_timeout_t timeout)
{
	int id;
	int rc = can_join(&id);

	if (rc != 0) {
		return rc;
	}

	struct can_frame frame;

	if (k_msgq_get(&can_rx_q[id], &frame, timeout) != 0) {
		return -EAGAIN;
	}

	memset(out, 0, sizeof(*out));
	out->id = frame.id;
	out->dlc = MIN(frame.dlc, (uint8_t)CAN_PAYLOAD_MAX);
	if ((frame.flags & CAN_FRAME_IDE) != 0U) {
		out->flags |= APP_CAN_FRAME_EXT;
	}
	if ((frame.flags & CAN_FRAME_RTR) != 0U) {
		out->flags |= APP_CAN_FRAME_RTR;
	} else {
		memcpy(out->data, frame.data, out->dlc);
	}
	return 0;
}

static bool frame_valid(uint32_t id, uint8_t flags, uint8_t dlc)
{
	uint32_t max = ((flags & APP_CAN_FRAME_EXT) != 0U) ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK;

	return (flags & ~(APP_CAN_FRAME_EXT | APP_CAN_FRAME_RTR)) == 0U && id <= max &&
	       dlc <= CAN_PAYLOAD_MAX;
}
#endif

void app_platform_clear_can(int id)
{
#if DT_HAS_CHOSEN(zephyr_canbus)
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	atomic_clear_bit(&can_users, id);
	for (int i = 0; i < CAN_FILTERS; i++) {
		can_filters[id][i].used = false;
	}
	k_msgq_purge(&can_rx_q[id]);
	can_rx_dropped[id] = 0U;
	can_tx_error[id] = 0;
#else
	ARG_UNUSED(id);
#endif
}

int z_impl_app_svc_can_send(uint32_t id, const uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
	if (len > CAN_PAYLOAD_MAX) {
		return -EINVAL;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	struct app_can_frame frame = { .id = id, .dlc = (uint8_t)len };

	if (!frame_valid(id, 0U, (uint8_t)len)) {
		return -EINVAL;
	}
	memcpy(frame.data, buf, len);
	return can_send_one(&frame);
#else
	ARG_UNUSED(id);
	ARG_UNUSED(buf);
	return -ENODEV;
#endif
}

int z_impl_app_svc_can_recv(uint32_t *id_out, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	struct app_can_frame frame;
	int rc = can_recv_one(&frame, K_MSEC(timeout_ms));

	if (rc != 0) {
		return rc;
	}

	size_t n = MIN(cap, (size_t)frame.dlc);

	memcpy(buf, frame.data, n);
	*id_out = frame.id;
	return (int)n;
#else
	ARG_UNUSED(id_out);
	ARG_UNUSED(buf);
	ARG_UNUSED(cap);
	ARG_UNUSED(timeout_ms);
	return -ENODEV;
#endif
}

int z_impl_app_svc_can_send_frame(const struct app_can_frame *frame)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	if (!frame_valid(frame->id, frame->flags, frame->dlc)) {
		return -EINVAL;
	}
	return can_send_one(frame);
#else
	ARG_UNUSED(frame);
	return -ENODEV;
#endif
}

int z_impl_app_svc_can_recv_frame(struct app_can_frame *frame, int32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	return can_recv_one(frame, (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms));
#else
	ARG_UNUSED(frame);
	ARG_UNUSED(timeout_ms);
	return -ENODEV;
#endif
}

int z_impl_app_svc_can_filter_add(uint32_t id, uint32_t mask, uint8_t flags)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	int app;
	int rc;

	if (!frame_valid(id, flags, 0U) || !frame_valid(mask, flags, 0U)) {
		return -EINVAL;
	}
	rc = can_join(&app);
	if (rc != 0) {
		return rc;
	}
	if (app_service_lock(&can_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	rc = -ENOSPC;
	for (int i = 0; i < CAN_FILTERS; i++) {
		struct can_app_filter *f = &can_filters[app][i];

		if (f->used) {
			continue;
		}
		f->id = id;
		f->mask = mask;
		f->flags = 0U;
		if ((flags & APP_CAN_FRAME_EXT) != 0U) {
			f->flags |= CAN_FRAME_IDE;
		}
		if ((flags & APP_CAN_FRAME_RTR) != 0U) {
			f->flags |= CAN_FRAME_RTR;
		}
		f->used = true;
		rc = i;
		break;
	}
	app_service_unlock(&can_lock);
	return rc;
#else
	ARG_UNUSED(id);
	ARG_UNUSED(mask);
	ARG_UNUSED(flags);
	return -ENODEV;
#endif
}

int z_impl_app_svc_can_filter_remove(int filter)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || filter < 0 || filter >= CAN_FILTERS) {
		return -EINVAL;
	}
	if (!can_filters[id][filter].used) {
		return -ENOENT;
	}
	can_filters[id][filter].used = false;
	return 0;
#else
	ARG_UNUSED(filter);
	return -ENODEV;
#endif
}

int z_impl_app_svc_can_status(struct app_can_status *out)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	int id;
	int rc = can_join(&id);
	enum can_state state;
	struct can_bus_err_cnt cnt;

	if (rc != 0) {
		return rc;
	}
	rc = can_get_state(can_dev, &state, &cnt);
	if (rc != 0) {
		return rc;
	}

	memset(out, 0, sizeof(*out));
	switch (state) {
	case CAN_STATE_ERROR_ACTIVE:
		out->state = APP_CAN_ERROR_ACTIVE;
		break;
	case CAN_STATE_ERROR_WARNING:
		out->state = APP_CAN_ERROR_WARNING;
		break;
	case CAN_STATE_ERROR_PASSIVE:
		out->state = APP_CAN_ERROR_PASSIVE;
		break;
	case CAN_STATE_BUS_OFF:
		out->state = APP_CAN_BUS_OFF;
		break;
	default:
		out->state = APP_CAN_STOPPED;
		break;
	}
	out->tx_err_cnt = cnt.tx_err_cnt;
	out->rx_err_cnt = cnt.rx_err_cnt;
	out->last_tx_error = can_tx_error[id];
	out->rx_dropped = can_rx_dropped[id];
	can_tx_error[id] = 0;
	return 0;
#else
	ARG_UNUSED(out);
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_can_send(uint32_t id, const uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_app_svc_can_send(id, buf, len);
}
#include <zephyr/syscalls/app_svc_can_send_mrsh.c>

static inline int z_vrfy_app_svc_can_recv(uint32_t *id_out, uint8_t *buf, size_t cap,
					  uint32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(id_out, sizeof(*id_out)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	return z_impl_app_svc_can_recv(id_out, buf, cap, timeout_ms);
}
#include <zephyr/syscalls/app_svc_can_recv_mrsh.c>

static inline int z_vrfy_app_svc_can_send_frame(const struct app_can_frame *frame)
{
	struct app_can_frame copy;

	K_OOPS(k_usermode_from_copy(&copy, frame, sizeof(copy)));
	return z_impl_app_svc_can_send_frame(&copy);
}
#include <zephyr/syscalls/app_svc_can_send_frame_mrsh.c>

static inline int z_vrfy_app_svc_can_recv_frame(struct app_can_frame *frame,
						int32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(frame, sizeof(*frame)));
	return z_impl_app_svc_can_recv_frame(frame, timeout_ms);
}
#include <zephyr/syscalls/app_svc_can_recv_frame_mrsh.c>

static inline int z_vrfy_app_svc_can_filter_add(uint32_t id, uint32_t mask, uint8_t flags)
{
	return z_impl_app_svc_can_filter_add(id, mask, flags);
}
#include <zephyr/syscalls/app_svc_can_filter_add_mrsh.c>

static inline int z_vrfy_app_svc_can_filter_remove(int filter)
{
	return z_impl_app_svc_can_filter_remove(filter);
}
#include <zephyr/syscalls/app_svc_can_filter_remove_mrsh.c>

static inline int z_vrfy_app_svc_can_status(struct app_can_status *out)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_app_svc_can_status(out);
}
#include <zephyr/syscalls/app_svc_can_status_mrsh.c>
#endif
