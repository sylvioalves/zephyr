/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A loadable app that benchmarks the messaging path from user mode. It times
 * tight loops of send and receive using the platform cycle counter and reports
 * raw cycle totals; the host converts them to latency and throughput. This
 * measures the real cost an isolated app pays: the syscall crossing, the bounds
 * check, the copy and the message queue.
 */

#include <zephyr/app_platform/app_sdk.h>

/* Append an unsigned decimal to a string buffer. */
static int put_u32(char *out, int i, uint32_t v)
{
	char tmp[10];
	int n = 0;

	if (v == 0) {
		out[i++] = '0';
		return i;
	}
	while (v > 0) {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	}
	while (n > 0) {
		out[i++] = tmp[--n];
	}
	return i;
}

static void report(const char *tag, uint32_t iters, uint32_t cycles)
{
	char line[64];
	int i = 0;

	while (*tag) {
		line[i++] = *tag++;
	}
	line[i++] = ' '; line[i++] = 'n'; line[i++] = '=';
	i = put_u32(line, i, iters);
	line[i++] = ' '; line[i++] = 'c'; line[i++] = 'y'; line[i++] = 'c'; line[i++] = '=';
	i = put_u32(line, i, cycles);
	line[i] = '\0';
	app_svc_log(line);
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	app_svc_ipc_register("bench");
	app_svc_topic_subscribe("bench/topic");

	uint8_t msg[16] = { 'b', 'e', 'n', 'c', 'h', 0 };
	uint8_t rx[16];

	app_svc_log("bench: warming up");
	for (int i = 0; i < 100; i++) {
		app_svc_ipc_send(0, msg, 8);
		app_svc_ipc_recv(rx, sizeof(rx), 10);
	}

	/* 1. Round-trip: send to own mailbox then receive (syscall+copy+msgq). */
	const uint32_t N = 2000;
	uint32_t t0 = app_svc_cycles();

	for (uint32_t i = 0; i < N; i++) {
		app_svc_ipc_send(0, msg, 8);
		app_svc_ipc_recv(rx, sizeof(rx), 10);
	}
	report("rtt8", N, app_svc_cycles() - t0);

	/* 2. Send-only throughput, 8-byte payload (drain to avoid full queue). */
	t0 = app_svc_cycles();
	for (uint32_t i = 0; i < N; i++) {
		app_svc_ipc_send(0, msg, 8);
		app_svc_ipc_recv(rx, sizeof(rx), 10);
	}
	uint32_t send8 = app_svc_cycles() - t0;

	report("send8", N, send8);

	/* 3. Larger payload (64 bytes) to expose copy cost. */
	uint8_t big[64];

	for (int i = 0; i < 64; i++) {
		big[i] = (uint8_t)i;
	}
	t0 = app_svc_cycles();
	for (uint32_t i = 0; i < N; i++) {
		app_svc_ipc_send(0, big, 64);
		app_svc_ipc_recv(rx, sizeof(rx), 10);
	}
	report("rtt64", N, app_svc_cycles() - t0);

	/* 4. Bare syscall cost baseline: app_svc_cycles itself, N times. */
	t0 = app_svc_cycles();
	volatile uint32_t sink = 0;

	for (uint32_t i = 0; i < N; i++) {
		sink += app_svc_cycles();
	}
	report("syscall", N, app_svc_cycles() - t0);

	/* 5. Topic publish round-trip (publish then recv the broadcast). */
	t0 = app_svc_cycles();
	for (uint32_t i = 0; i < N; i++) {
		app_svc_topic_publish("bench/topic", msg, 8);
		app_svc_ipc_recv(rx, sizeof(rx), 10);
	}
	report("pub8", N, app_svc_cycles() - t0);

	app_svc_log("bench: done");
	for (;;) {
		app_svc_heartbeat();
		app_sleep_ms(1000);
	}
}
EXPORT_SYMBOL(app_main);
