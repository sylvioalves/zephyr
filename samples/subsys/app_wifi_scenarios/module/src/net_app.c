/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Loadable application that exercises the shared Wi-Fi link from inside the
 * sandbox: it brings the link up, runs a TCP round trip against an echo server,
 * then watches the link and reports every transition. Powering the access point
 * off and on during the watch is what proves the platform notices the loss and
 * recovers on its own; the application holds no socket and links none of the
 * network stack.
 *
 * The echo server's address is compiled in (ECHO_HOST/ECHO_PORT).
 */

#include <zephyr/app_platform/app_services.h>

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifndef ECHO_HOST
#define ECHO_HOST "192.168.15.11"
#endif
#ifndef ECHO_PORT
#define ECHO_PORT 9099
#endif

/* Watch this many seconds after the round trip, reporting each transition. */
#define WATCH_SECONDS 120

/* Render a small signed decimal without pulling libc into the module. */
static char *put_int(char *p, char *end, int v)
{
	char tmp[12];
	int n = 0;
	unsigned int u = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;

	if (v < 0 && p < end) {
		*p++ = '-';
	}
	do {
		tmp[n++] = (char)('0' + (u % 10U));
		u /= 10U;
	} while (u > 0U && n < (int)sizeof(tmp));
	while (n > 0 && p < end) {
		*p++ = tmp[--n];
	}
	return p;
}

static char *put_str(char *p, char *end, const char *s)
{
	while (*s != '\0' && p < end) {
		*p++ = *s++;
	}
	return p;
}

static void report(const char *tag, int value, const char *suffix)
{
	char line[96];
	char *p = line;
	char *end = line + sizeof(line) - 1;

	p = put_str(p, end, tag);
	p = put_int(p, end, value);
	p = put_str(p, end, suffix);
	*p = '\0';
	app_svc_log(line);
}

static void tcp_round_trip(void)
{
	char rx[64];
	static const char msg[] = "app_platform tcp probe";
	uint32_t t0 = app_svc_uptime_ms();
	int rc = app_svc_tcp_connect(ECHO_HOST, ECHO_PORT);

	if (rc != 0) {
		report("tcp: connect failed rc=", rc, "");
		return;
	}
	report("tcp: connected in ", (int)(app_svc_uptime_ms() - t0), " ms");

	rc = app_svc_tcp_send(msg, sizeof(msg) - 1);
	report("tcp: sent rc=", rc, "");

	rc = app_svc_tcp_recv(rx, sizeof(rx) - 1, 5000);
	if (rc > 0) {
		rx[rc] = '\0';
		report("tcp: echoed ", rc, " bytes");
		app_svc_log(rx);
	} else {
		report("tcp: recv rc=", rc, "");
	}
	(void)app_svc_tcp_close();
	report("tcp: round trip ", (int)(app_svc_uptime_ms() - t0), " ms");
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	uint32_t t0 = app_svc_uptime_ms();
	int rc = app_svc_net_ready();

	report("net: link up rc=", rc, "");
	report("net: took ", (int)(app_svc_uptime_ms() - t0), " ms");

	if (rc == 0) {
		tcp_round_trip();
	}

	app_svc_log("watch: power the access point off and on now");
	int last = -1;
	uint32_t tw = app_svc_uptime_ms();

	for (int i = 0; i < WATCH_SECONDS; i++) {
		struct app_svc_wifi_status ws;
		int now = (app_svc_wifi_status(&ws) == 0 && ws.connected) ? 1 : 0;

		if (now != last) {
			report(now ? "watch: link UP at " : "watch: link DOWN at ",
			       (int)(app_svc_uptime_ms() - tw), " ms");
			last = now;
		}
		k_msleep(1000);
	}
	app_svc_log("watch: done");

	for (;;) {
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
