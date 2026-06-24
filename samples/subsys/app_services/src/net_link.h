/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SERVICES_NET_LINK_H_
#define APP_SERVICES_NET_LINK_H_

#include <zephyr/kernel.h>

/*
 * Network connectivity broker. The shared Wi-Fi radio and IP stack live in the
 * trusted base: the supervisor connects to Wi-Fi and runs DHCP. Once an IP is
 * obtained, apps are released to use their OWN sockets independently (sockets
 * are user-mode syscalls, isolated per-app by kobject permission). This mirrors
 * how a phone OS owns the modem while apps each open their own connections.
 */

/**
 * @brief Bring up Wi-Fi + DHCP (supervisor context). Returns 0 on link up.
 */
int net_link_up(void);

/**
 * @brief Block until the network is ready (IP obtained), or timeout.
 * Apps wait on this (via a granted semaphore) before opening sockets.
 */
int net_link_wait_ready(k_timeout_t timeout);

/* The "network ready" semaphore apps are granted and wait on. */
extern struct k_sem net_ready_sem;

/* The assigned gateway address (4 bytes), valid once the link is ready. An
 * on-subnet, always-routable target the apps can independently connect to.
 */
extern uint8_t net_link_gateway[4];

#endif /* APP_SERVICES_NET_LINK_H_ */
