/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES14_SVC_H_
#define APP_MORE_SERVICES14_SVC_H_

#include <zephyr/kernel.h>

/*
 * A fourteenth batch: an IEEE 802.15.4 radio messaging service. The platform
 * owns the 802.15.4 interface, the net stack, and a UDP socket bound to a peer
 * over the radio. The app sends and receives raw payloads from user mode; the
 * supervisor carries them over 802.15.4 to the peer and back, the same brokered
 * shape used for MQTT but over the radio instead of Wi-Fi.
 *
 *   svc_radio_send - send @len payload bytes to the peer over 802.15.4.
 *   svc_radio_recv - poll the next received payload; -EAGAIN if none waiting.
 */

/* Scoped: send @buf[0..len) to the peer over the radio. Returns bytes sent. */
__syscall int svc_radio_send(const uint8_t *buf, size_t len);

/* Scoped: poll the next received payload into @buf (cap @cap); -EAGAIN if none. */
__syscall int svc_radio_recv(uint8_t *buf, size_t cap, int32_t timeout_ms);

/* Supervisor-only: bring up the radio socket to the peer before the app runs. */
int svc_radio_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES14_SVC_H_ */
