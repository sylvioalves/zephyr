/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The shared 802.15.4 messaging service. The platform owns the radio, the IPv6
 * stack and the UDP socket; an application that holds APP_CAP_RADIO sends and
 * receives datagrams to a peer through copied buffers and never holds the socket
 * (a socket is a kernel object a user thread cannot keep). A supervisor pair of
 * threads runs all socket I/O: a tx thread drains an outgoing queue, an rx
 * thread fills an incoming queue.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <errno.h>
#include "app_grants.h"

#define PEER_ADDR CONFIG_APP_PLATFORM_RADIO_PEER
#define PEER_PORT 4242
#define MSG_MAX   64

struct radio_msg {
	uint16_t len;
	uint8_t  data[MSG_MAX];
};

K_MSGQ_DEFINE(radio_tx_q, sizeof(struct radio_msg), 4, 4);
K_MSGQ_DEFINE(radio_rx_q, sizeof(struct radio_msg), 4, 4);

static int radio_sock = -1;
static struct sockaddr_in6 peer;
static K_SEM_DEFINE(radio_up, 0, 1);

static int radio_open(void)
{
	radio_sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (radio_sock < 0) {
		return -errno;
	}

	/* Bind the local port so datagrams the peer sends to us are delivered,
	 * not just replies that happen to match an ephemeral source port.
	 */
	struct sockaddr_in6 local = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(PEER_PORT),
		.sin6_addr = in6addr_any,
	};

	if (zsock_bind(radio_sock, (struct sockaddr *)&local,
		       sizeof(local)) < 0) {
		int rc = -errno;

		zsock_close(radio_sock);
		radio_sock = -1;
		return rc;
	}

	peer.sin6_family = AF_INET6;
	peer.sin6_port = htons(PEER_PORT);
	if (zsock_inet_pton(AF_INET6, PEER_ADDR, &peer.sin6_addr) != 1) {
		return -EINVAL;
	}
	return 0;
}

/* Supervisor tx loop: send queued payloads to the peer. */
static void radio_tx(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	k_sem_take(&radio_up, K_FOREVER);
	for (;;) {
		struct radio_msg m;

		if (k_msgq_get(&radio_tx_q, &m, K_FOREVER) == 0) {
			zsock_sendto(radio_sock, m.data, m.len, 0,
				     (struct sockaddr *)&peer, sizeof(peer));
		}
	}
}

/* Supervisor rx loop: receive datagrams into the incoming queue. */
static void radio_rx(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	k_sem_take(&radio_up, K_FOREVER);
	for (;;) {
		struct radio_msg m;
		int n = zsock_recv(radio_sock, m.data, sizeof(m.data), 0);

		if (n > 0) {
			m.len = (uint16_t)n;
			(void)k_msgq_put(&radio_rx_q, &m, K_NO_WAIT);
		}
	}
}

K_THREAD_DEFINE(radio_tx_tid, 2048, radio_tx, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(radio_rx_tid, 2048, radio_rx, NULL, NULL, NULL, 7, 0, 0);

static K_MUTEX_DEFINE(radio_lock);
static bool radio_ready;

static int radio_ensure_up(void)
{
	int rc = 0;

	if (app_svc_lock(&radio_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	if (!radio_ready) {
		rc = radio_open();
		if (rc == 0) {
			radio_ready = true;
			k_sem_give(&radio_up);
			k_sem_give(&radio_up);
		}
	}
	app_svc_unlock(&radio_lock);
	return rc;
}

int z_impl_svc_radio_send(const void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_RADIO)) {
		return -EPERM;
	}
	if (len > MSG_MAX) {
		return -EINVAL;
	}
	if (radio_ensure_up() != 0) {
		return -ENETDOWN;
	}

	struct radio_msg m;

	m.len = (uint16_t)len;
	memcpy(m.data, buf, len);
	return k_msgq_put(&radio_tx_q, &m, K_NO_WAIT) == 0 ? 0 : -EAGAIN;
}

int z_impl_svc_radio_recv(void *buf, size_t len, uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_RADIO)) {
		return -EPERM;
	}
	if (radio_ensure_up() != 0) {
		return -ENETDOWN;
	}

	struct radio_msg m;

	if (k_msgq_get(&radio_rx_q, &m, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
	}

	size_t n = MIN(len, (size_t)m.len);

	memcpy(buf, m.data, n);
	return (int)n;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_radio_send(const void *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_radio_send(buf, len);
}
#include <zephyr/syscalls/svc_radio_send_mrsh.c>

static inline int z_vrfy_svc_radio_recv(void *buf, size_t len, uint32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_radio_recv(buf, len, timeout_ms);
}
#include <zephyr/syscalls/svc_radio_recv_mrsh.c>
#endif
