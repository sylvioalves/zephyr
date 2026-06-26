/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <errno.h>

#define PAYLOAD_MAX 64
#define PEER_ADDR   "2001:db8::1"
#define PEER_PORT   4242

/* The platform owns the radio socket; the app only hands over payloads. */
static int sock = -1;
static struct sockaddr_in6 peer;

/*
 * All socket I/O runs in supervisor threads so the user app never holds the
 * socket kobject. The app hands payloads to a tx queue and reads from an rx
 * queue; the platform's threads move them on and off the radio socket.
 */
struct rx_msg {
	uint8_t data[PAYLOAD_MAX];
	uint16_t len;
};
K_MSGQ_DEFINE(rx_q, sizeof(struct rx_msg), 8, 4);
K_MSGQ_DEFINE(tx_q, sizeof(struct rx_msg), 8, 4);

static K_THREAD_STACK_DEFINE(rx_stack, 2048);
static K_THREAD_STACK_DEFINE(tx_stack, 2048);
static struct k_thread rx_thread;
static struct k_thread tx_thread;

static void rx_loop(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (true) {
		struct rx_msg m;
		int n = zsock_recv(sock, m.data, sizeof(m.data), 0);

		if (n > 0) {
			m.len = (uint16_t)n;
			(void)k_msgq_put(&rx_q, &m, K_NO_WAIT);
		} else {
			k_msleep(20);
		}
	}
}

static void tx_loop(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (true) {
		struct rx_msg m;

		if (k_msgq_get(&tx_q, &m, K_FOREVER) == 0) {
			(void)zsock_send(sock, m.data, m.len, 0);
		}
	}
}

int svc_radio_install(void)
{
	sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}

	peer.sin6_family = AF_INET6;
	peer.sin6_port = htons(PEER_PORT);
	if (zsock_inet_pton(AF_INET6, PEER_ADDR, &peer.sin6_addr) != 1) {
		return -EINVAL;
	}

	if (zsock_connect(sock, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
		return -errno;
	}

	k_thread_create(&rx_thread, rx_stack, K_THREAD_STACK_SIZEOF(rx_stack),
			rx_loop, NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_create(&tx_thread, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack),
			tx_loop, NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
	return 0;
}

int z_impl_svc_radio_send(const uint8_t *buf, size_t len)
{
	if (sock < 0 || len == 0U || len > PAYLOAD_MAX) {
		return -EINVAL;
	}

	struct rx_msg m;

	m.len = (uint16_t)len;
	memcpy(m.data, buf, len);
	if (k_msgq_put(&tx_q, &m, K_MSEC(100)) != 0) {
		return -EAGAIN;
	}
	return (int)len;
}

int z_impl_svc_radio_recv(uint8_t *buf, size_t cap, int32_t timeout_ms)
{
	struct rx_msg m;
	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	if (k_msgq_get(&rx_q, &m, t) != 0) {
		return -EAGAIN;
	}

	size_t n = MIN((size_t)m.len, cap);

	memcpy(buf, m.data, n);
	return (int)n;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_radio_send(const uint8_t *buf, size_t len)
{
	if (len > PAYLOAD_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_radio_send(buf, len);
}
#include <zephyr/syscalls/svc_radio_send_mrsh.c>

static inline int z_vrfy_svc_radio_recv(uint8_t *buf, size_t cap, int32_t timeout_ms)
{
	if (cap > PAYLOAD_MAX) {
		cap = PAYLOAD_MAX;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	return z_impl_svc_radio_recv(buf, cap, timeout_ms);
}
#include <zephyr/syscalls/svc_radio_recv_mrsh.c>
#endif
