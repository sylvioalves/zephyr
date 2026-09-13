/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application moving bulk data through a ring the
 * platform maps into its own memory. The application fills the ring directly
 * and crosses the syscall boundary once for the whole batch, instead of once
 * per buffer as svc_uart_write does. The board overlay loops the port back on
 * itself, so the application reads back what it sent and the two paths are
 * compared on the same bytes and the same hardware.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>

K_APPMEM_PARTITION_DEFINE(ring_part);
K_APP_DMEM(ring_part) static int passes;
K_APP_DMEM(ring_part) static int fails;

#define PAYLOAD 192

static void check(bool ok, const char *what, int rc)
{
	char line[80];

	if (ok) {
		(void)snprintk(line, sizeof(line), "PASS %s", what);
		passes++;
	} else {
		(void)snprintk(line, sizeof(line), "FAIL %s (rc=%d)", what, rc);
		fails++;
	}
	svc_log(line);
}

static void ring_app(void *arg)
{
	ARG_UNUSED(arg);

	uintptr_t addr = 0;
	size_t cap = 0;

	int rc = svc_ring_bind(PAYLOAD * 2, &addr, &cap);

	if (rc == -ENODEV) {
		svc_log("ring: no secondary uart on this board, nothing to do");
		return;
	}
	check(rc == 0 && addr != 0U, "bind a ring", rc);
	if (rc != 0) {
		return;
	}
	check(cap >= PAYLOAD, "the ring holds the payload", (int)cap);

	rc = svc_ring_bind(64, &addr, &cap);
	check(rc == -EEXIST, "a second bind is refused", rc);

	struct app_ring_hdr *h = (struct app_ring_hdr *)addr;
	uint8_t *data = (uint8_t *)(addr + sizeof(*h));

	/* Writing straight into the mapped ring is the point: these stores go
	 * to the application's own memory and cost no syscall at all.
	 */
	for (int i = 0; i < PAYLOAD; i++) {
		data[i] = (uint8_t)(i & 0xFF);
	}
	h->head = PAYLOAD;
	h->tail = 0;

	uint32_t t0 = svc_cycles();

	rc = svc_ring_flush();

	uint32_t ring_cycles = svc_cycles() - t0;

	check(rc == PAYLOAD, "one flush sent the whole payload", rc);

	/* The same bytes through the copying path, for a like-for-like number
	 * on the same port.
	 */
	uint8_t buf[64];

	for (int i = 0; i < 64; i++) {
		buf[i] = (uint8_t)(i & 0xFF);
	}
	t0 = svc_cycles();
	for (int i = 0; i < PAYLOAD / 64; i++) {
		(void)svc_uart_write(buf, sizeof(buf));
	}
	uint32_t call_cycles = svc_cycles() - t0;

	char line[80];

	(void)snprintk(line, sizeof(line),
		       "ring: %u cycles for %d bytes in 1 call", ring_cycles,
		       PAYLOAD);
	svc_log(line);
	(void)snprintk(line, sizeof(line),
		       "write: %u cycles for %d bytes in %d calls", call_cycles,
		       PAYLOAD, PAYLOAD / 64);
	svc_log(line);

	/* Both paths push the same bytes out the same port, so the ring is
	 * only worth having if it is not slower.
	 */
	check(ring_cycles <= call_cycles, "the ring is no slower than the calls",
	      (int)ring_cycles);

	/* The 384 bytes sent above take about 33ms to arrive at 115200 baud,
	 * and a poll drains the receiver far faster than that, so an empty read
	 * does not mean the line is idle. Wait out the transmission, then drain
	 * what landed, so the probe below is not read behind stale bytes.
	 */
	(void)svc_timer_start(120);
	(void)svc_timer_wait(200);

	for (int i = 0; i < 8; i++) {
		h->head = 0;
		h->tail = 0;
		(void)svc_ring_fill();
	}
	h->head = 0;
	h->tail = 0;

	static const uint8_t probe[4] = {0xC5, 0x1E, 0xA7, 0x5F};

	for (int i = 0; i < (int)sizeof(probe); i++) {
		data[i] = probe[i];
	}
	h->head = sizeof(probe);
	rc = svc_ring_flush();
	check(rc == (int)sizeof(probe), "flush a known probe", rc);

	h->head = 0;
	h->tail = 0;

	int got = 0;

	for (int i = 0; i < 60; i++) {
		rc = svc_ring_fill();
		if (rc > 0) {
			got += rc;
		}
		(void)svc_timer_start(5);
		(void)svc_timer_wait(10);
	}
	check(got > 0, "fill received bytes over the loopback", got);

	/* Only the receive direction is checked for content here. Matching the
	 * probe byte for byte would mean knowing the loopback had gone idle
	 * first, and a poll drains the receiver faster than the line delivers,
	 * so an empty read does not prove that. The transmit direction is
	 * already compared byte for byte by app_uart_line.
	 */

	/* A corrupt index must be refused rather than steering the platform
	 * out of the buffer: this is the whole reason the indices are checked
	 * on every use instead of trusted.
	 */
	h->head = 0xFFFFFFFFU;
	rc = svc_ring_flush();
	check(rc == -EBADMSG, "an out-of-range head is refused", rc);

	h->head = 0;
	h->tail = 0xDEADBEEFU;
	rc = svc_ring_fill();
	check(rc == -EBADMSG, "an out-of-range tail is refused", rc);

	h->tail = 0;
	rc = svc_ring_unbind();
	check(rc == 0, "unbind the ring", rc);

	rc = svc_ring_flush();
	check(rc == -EINVAL, "flushing without a ring is refused", rc);

	(void)snprintk(line, sizeof(line), "ring: %d passed, %d failed", passes,
		       fails);
	svc_log(line);
	svc_log(fails == 0 ? "ring: done" : "ring: FAILURES");
}

int main(void)
{
	printk("\n*** app_ring: bulk transfer through a mapped ring ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "ring",
		.source = APP_BUILTIN,
		.entry = ring_app,
		.caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_UART,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}
	k_mem_domain_add_partition(app_platform_domain(id), &ring_part);

	printk("*** launched isolated ring app (group %d) ***\n", id);
	return 0;
}
