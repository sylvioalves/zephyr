/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The socket services over loopback: an application trades datagrams and a
 * stream with echo servers in the firmware through the net service, and the
 * suite checks the data, the receive timeout, the release of the socket on
 * reclaim and the time of a round trip.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_context.h>
#include <errno.h>

#include "throughput.h"

#define UDP_PORT   4242
#define TCP_PORT   4243
#define IDLE_PORT  4244
#define DGRAM_MAX  CONFIG_APP_PLATFORM_NET_DGRAM_MAX
#define ROUNDS     32
#define NET_CAPS   (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_NET | APP_CAP_SOCKET)

/* Upper bound on the median round trip, far below one poll period of a net
 * thread that polls its sockets.
 */
#define RTT_BOUND_US 5000

K_APP_DMEM(report_part) volatile int net_rc = -9999;
K_APP_DMEM(report_part) volatile int net_done;
K_APP_DMEM(report_part) volatile int net_holding;
K_APP_DMEM(report_part) volatile int net_release;
K_APP_DMEM(report_part) volatile uint32_t net_elapsed_ms;
K_APP_DMEM(report_part) uint32_t rtt[ROUNDS];

static void udp_echo(void *a, void *b, void *c)
{
	static uint8_t buf[DGRAM_MAX];
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(UDP_PORT),
	};
	int fd = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	(void)zsock_inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (fd < 0 || zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		printk("udp echo: no socket (%d)\n", errno);
		return;
	}
	for (;;) {
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		ssize_t n = zsock_recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from,
					   &fromlen);

		if (n > 0) {
			(void)zsock_sendto(fd, buf, n, 0, (struct sockaddr *)&from, fromlen);
		}
	}
}

K_THREAD_DEFINE(udp_echo_tid, 2048, udp_echo, NULL, NULL, NULL, 5, 0, 0);

static void tcp_echo(void *a, void *b, void *c)
{
	static uint8_t buf[DGRAM_MAX];
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(TCP_PORT),
	};
	int lfd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	(void)zsock_inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (lfd < 0 || zsock_bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    zsock_listen(lfd, 1) != 0) {
		printk("tcp echo: no socket (%d)\n", errno);
		return;
	}
	for (;;) {
		int fd = zsock_accept(lfd, NULL, NULL);
		ssize_t n;

		if (fd < 0) {
			continue;
		}
		while ((n = zsock_recv(fd, buf, sizeof(buf), 0)) > 0) {
			ssize_t off = 0;

			while (off < n) {
				ssize_t m = zsock_send(fd, buf + off, n - off, 0);

				if (m <= 0) {
					break;
				}
				off += m;
			}
		}
		(void)zsock_close(fd);
	}
}

K_THREAD_DEFINE(tcp_echo_tid, 2048, tcp_echo, NULL, NULL, NULL, 5, 0, 0);

static size_t round_len(int i)
{
	return 1U + ((size_t)i * 37U) % DGRAM_MAX;
}

static void fill(uint8_t *buf, size_t len, int seed)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)((uint32_t)seed * 31U + i);
	}
}

static bool same(const uint8_t *a, const uint8_t *b, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

static void report(int rc)
{
	net_rc = rc;
	net_done = 1;
}

static int udp_round(int i, uint8_t *tx, uint8_t *rx, uint32_t *cycles)
{
	size_t len = round_len(i);
	uint32_t t0;
	int n;

	fill(tx, len, i);
	t0 = app_svc_cycles();
	n = app_svc_udp_send(tx, len);
	if (n != (int)len) {
		return (n < 0) ? n : -EIO;
	}
	n = app_svc_udp_recv(rx, DGRAM_MAX, 1000);
	*cycles = app_svc_cycles() - t0;
	if (n < 0) {
		return n;
	}
	if (n != (int)len || !same(tx, rx, len)) {
		return -EBADMSG;
	}
	return 0;
}

static void udp_rtt_app(void *arg)
{
	uint8_t tx[DGRAM_MAX];
	uint8_t rx[DGRAM_MAX];
	uint32_t cycles = 0U;
	int rc = app_svc_net_ready();

	ARG_UNUSED(arg);

	if (rc == 0) {
		rc = app_svc_udp_open("127.0.0.1", UDP_PORT);
	}
	/* The first round warms up the path and is not timed. */
	for (int i = 0; rc == 0 && i <= ROUNDS; i++) {
		rc = udp_round(i, tx, rx, &cycles);
		if (i > 0) {
			rtt[i - 1] = cycles;
		}
	}
	(void)app_svc_udp_close();
	report(rc);
}

static void tcp_rtt_app(void *arg)
{
	uint8_t tx[DGRAM_MAX];
	uint8_t rx[DGRAM_MAX];
	int rc = app_svc_net_ready();

	ARG_UNUSED(arg);

	if (rc == 0) {
		rc = app_svc_tcp_connect("127.0.0.1", TCP_PORT);
	}
	for (int i = 0; rc == 0 && i <= ROUNDS; i++) {
		size_t len = round_len(i);
		size_t got = 0U;
		uint32_t t0;
		int n;

		fill(tx, len, i);
		t0 = app_svc_cycles();
		n = app_svc_tcp_send(tx, len);
		if (n != (int)len) {
			rc = (n < 0) ? n : -EIO;
			break;
		}
		while (got < len) {
			n = app_svc_tcp_recv(rx + got, len - got, 1000);
			if (n <= 0) {
				rc = (n < 0) ? n : -ECONNRESET;
				break;
			}
			got += (size_t)n;
		}
		if (i > 0) {
			rtt[i - 1] = app_svc_cycles() - t0;
		}
		if (rc == 0 && !same(tx, rx, len)) {
			rc = -EBADMSG;
		}
	}
	(void)app_svc_tcp_close();
	report(rc);
}

static void udp_timeout_app(void *arg)
{
	uint8_t rx[16];
	uint32_t t0;
	int rc = app_svc_net_ready();

	ARG_UNUSED(arg);

	if (rc == 0) {
		rc = app_svc_udp_open("127.0.0.1", IDLE_PORT);
	}
	if (rc == 0) {
		t0 = app_svc_uptime_ms();
		rc = app_svc_udp_recv(rx, sizeof(rx), 50);
		net_elapsed_ms = app_svc_uptime_ms() - t0;
	}
	(void)app_svc_udp_close();
	report(rc);
}

static void udp_held_app(void *arg)
{
	uint8_t tx[4] = { 1, 2, 3, 4 };
	uint8_t rx[4];
	int rc = app_svc_net_ready();

	ARG_UNUSED(arg);

	if (rc == 0) {
		rc = app_svc_udp_open("127.0.0.1", UDP_PORT);
	}
	if (rc == 0) {
		rc = app_svc_udp_send(tx, sizeof(tx));
		rc = (rc == (int)sizeof(tx)) ? 0 : -EIO;
	}
	if (rc == 0) {
		rc = app_svc_udp_recv(rx, sizeof(rx), 1000);
		rc = (rc == (int)sizeof(rx)) ? 0 : -EIO;
	}
	report(rc);
	net_holding = 1;
	while (net_release == 0) {
		k_msleep(1);
	}
}

static void count_used(struct net_context *ctx, void *user_data)
{
	int *count = user_data;

	if (net_context_is_used(ctx)) {
		(*count)++;
	}
}

static int contexts_used(void)
{
	int count = 0;

	net_context_foreach(count_used, &count);
	return count;
}

static void run_app(const char *name, app_entry_t entry)
{
	net_rc = -9999;
	net_done = 0;
	(void)tp_launch(name, entry, NET_CAPS);
	zassert_true(tp_wait_gone(name, 20000), "%s never exited", name);
	zassert_equal(net_done, 1, "%s did not report", name);
}

static uint32_t report_rtt(const char *what)
{
	uint32_t median;

	tp_sort(rtt, ROUNDS);
	median = rtt[ROUNDS / 2];
	TC_PRINT("%s round trip: median %u cycles (%u us), min %u, max %u\n", what, median,
		 k_cyc_to_us_ceil32(median), rtt[0], rtt[ROUNDS - 1]);
	return median;
}

ZTEST(net, test_udp_round_trip)
{
	uint32_t median;

	run_app("udp_rtt", udp_rtt_app);
	zassert_equal(net_rc, 0, "udp round trips failed: %d", net_rc);
	median = report_rtt("udp");
	zassert_true(k_cyc_to_us_ceil32(median) < RTT_BOUND_US, "udp round trip %u us",
		     k_cyc_to_us_ceil32(median));
}

ZTEST(net, test_tcp_round_trip)
{
	uint32_t median;

	run_app("tcp_rtt", tcp_rtt_app);
	zassert_equal(net_rc, 0, "tcp round trips failed: %d", net_rc);
	median = report_rtt("tcp");
	zassert_true(k_cyc_to_us_ceil32(median) < RTT_BOUND_US, "tcp round trip %u us",
		     k_cyc_to_us_ceil32(median));
}

ZTEST(net, test_udp_recv_times_out)
{
	run_app("udp_idle", udp_timeout_app);
	zassert_equal(net_rc, -EAGAIN, "recv on an idle socket returned %d", net_rc);
	zassert_true(net_elapsed_ms >= 45U && net_elapsed_ms < 150U,
		     "recv timeout of 50 ms took %u ms", net_elapsed_ms);
}

ZTEST(net, test_udp_socket_closed_on_reclaim)
{
	int base = contexts_used();
	int id;

	net_rc = -9999;
	net_done = 0;
	net_holding = 0;
	net_release = 0;
	id = tp_launch("udp_held", udp_held_app, NET_CAPS);
	for (int i = 0; i < 2000 && net_holding == 0; i++) {
		k_msleep(5);
	}
	zassert_equal(net_holding, 1, "udp_held never reported");
	zassert_equal(net_rc, 0, "udp_held rc %d", net_rc);
	zassert_equal(contexts_used(), base + 1, "the socket of udp_held is not open");

	zassert_equal(app_platform_remove(id), 0);
	zassert_true(tp_wait_gone("udp_held", 2000), "udp_held never reclaimed");
	for (int i = 0; i < 200 && contexts_used() != base; i++) {
		k_msleep(5);
	}
	zassert_equal(contexts_used(), base, "reclaim left the socket of udp_held open");
	net_release = 1;
}
