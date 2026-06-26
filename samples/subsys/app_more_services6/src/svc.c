/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <errno.h>

#define CAN_PAYLOAD_MAX 8

/* The platform owns the CAN controller; the app reaches it through this seam. */
static const struct device *const can_dev = DEVICE_DT_GET(DT_NODELABEL(twai0));

/* Per-app receive queue: the platform's filter feeds frames here in S-mode. */
K_MSGQ_DEFINE(app_can_q, sizeof(struct can_frame), 8, 4);

void svc_can_install(void)
{
	if (!device_is_ready(can_dev)) {
		return;
	}

	/* Loopback self-test: a sent frame is received back, no bus needed. */
	(void)can_set_mode(can_dev, CAN_MODE_LOOPBACK);

	/* Accept every standard id; in a real grant this mask is scoped. */
	const struct can_filter filter = {
		.id = 0,
		.mask = 0,
		.flags = 0,
	};

	(void)can_add_rx_filter_msgq(can_dev, &app_can_q, &filter);
	(void)can_start(can_dev);
}

int z_impl_svc_can_send(uint32_t id, const uint8_t *buf, size_t len)
{
	if (!device_is_ready(can_dev) || len > CAN_PAYLOAD_MAX) {
		return -EINVAL;
	}

	struct can_frame frame = {
		.id = id,
		.dlc = (uint8_t)len,
		.flags = 0,
	};

	memcpy(frame.data, buf, len);

	int rc = can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);

	return (rc == 0) ? (int)len : rc;
}

int z_impl_svc_can_recv(uint32_t *id_out, uint8_t *buf, size_t cap)
{
	struct can_frame frame;

	if (k_msgq_get(&app_can_q, &frame, K_NO_WAIT) != 0) {
		return -EAGAIN;
	}

	size_t n = MIN((size_t)frame.dlc, cap);

	*id_out = frame.id;
	memcpy(buf, frame.data, n);
	return (int)n;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_can_send(uint32_t id, const uint8_t *buf, size_t len)
{
	if (len > CAN_PAYLOAD_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_can_send(id, buf, len);
}
#include <zephyr/syscalls/svc_can_send_mrsh.c>

static inline int z_vrfy_svc_can_recv(uint32_t *id_out, uint8_t *buf, size_t cap)
{
	if (cap > CAN_PAYLOAD_MAX) {
		cap = CAN_PAYLOAD_MAX;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(id_out, sizeof(*id_out)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	return z_impl_svc_can_recv(id_out, buf, cap);
}
#include <zephyr/syscalls/svc_can_recv_mrsh.c>
#endif
