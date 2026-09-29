/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The shared CAN service. The platform owns the controller and runs it in
 * loopback so frames sent by an application come back to be received, letting
 * the path be exercised on a single board. An application holding APP_CAP_CAN
 * sends and receives frames through copied buffers and never holds the bus. A
 * receive queue filled by the controller decouples the application from the ISR.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/drivers/can.h>
#include <string.h>
#include <errno.h>
#include "app_grants.h"

#define CAN_PAYLOAD_MAX 8

#if DT_HAS_CHOSEN(zephyr_canbus)
static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(app_can_q, 8);
static bool can_ready;
static SVC_LOCK_DEFINE(can_lock);

static void can_tx_done(const struct device *dev, int error, void *arg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(error);
	ARG_UNUSED(arg);
}

static int can_ensure_up(void)
{
	if (can_ready) {
		return 0;
	}
	if (app_service_lock(&can_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	int rc = 0;

	if (!can_ready) {
		if (!device_is_ready(can_dev)) {
			rc = -ENODEV;
			goto out;
		}
		/* Mode must be set while stopped, then start the controller. */
		if (can_set_mode(can_dev, CAN_MODE_LOOPBACK) != 0 ||
		    can_start(can_dev) != 0) {
			rc = -EIO;
			goto out;
		}

		struct can_filter f = { .id = 0, .mask = 0, .flags = 0 };

		if (can_add_rx_filter_msgq(can_dev, &app_can_q, &f) < 0) {
			rc = -EIO;
			goto out;
		}
		can_ready = true;
	}
out:
	app_service_unlock(&can_lock);
	return rc;
}
#endif

int z_impl_app_svc_can_send(uint32_t id, const uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_CAN)) {
		return -EPERM;
	}
	if (len > CAN_PAYLOAD_MAX) {
		return -EINVAL;
	}
#if DT_HAS_CHOSEN(zephyr_canbus)
	if (can_ensure_up() != 0) {
		return -ENODEV;
	}
	struct can_frame frame = {
		.id = id,
		.dlc = (uint8_t)len,
	};

	memcpy(frame.data, buf, len);
	/* Non-blocking: a NULL callback would block until TX completes. */
	return can_send(can_dev, &frame, K_MSEC(100), can_tx_done, NULL);
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
	if (can_ensure_up() != 0) {
		return -ENODEV;
	}
	struct can_frame frame;

	if (k_msgq_get(&app_can_q, &frame, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
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
#endif
