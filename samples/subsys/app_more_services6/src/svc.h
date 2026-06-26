/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES6_SVC_H_
#define APP_MORE_SERVICES6_SVC_H_

#include <zephyr/kernel.h>

/*
 * A sixth batch: the CAN bus, scoped to a granted message-id filter. The
 * platform owns the controller, sets its mode and bitrate, and installs a
 * receive filter that feeds a per-app queue. The app sends frames and polls
 * received ones; it never touches the driver frame struct - only scalar id and
 * a byte payload cross the user/kernel boundary.
 *
 *   svc_can_send - send a standard-id frame with @len payload bytes.
 *   svc_can_recv - poll the next received frame for this app; -EAGAIN if none.
 */

/* Scoped: send a standard 11-bit @id frame carrying @buf[0..len). */
__syscall int svc_can_send(uint32_t id, const uint8_t *buf, size_t len);

/*
 * Scoped: poll the next received frame. On success returns the payload length,
 * writes the id to *@id_out and up to @cap bytes into @buf. -EAGAIN if none.
 */
__syscall int svc_can_recv(uint32_t *id_out, uint8_t *buf, size_t cap);

/* Supervisor-only: set the controller mode, install the filter, and start. */
void svc_can_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES6_SVC_H_ */
