/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * M2 demo: two memory-isolated apps each use the internet INDEPENDENTLY through
 * one shared Wi-Fi radio + IP stack on C6.
 *
 * The supervisor (trusted base) owns the radio: it connects to Wi-Fi and runs
 * DHCP, then releases the apps. Each app then opens its OWN socket and does its
 * OWN DNS lookup in user mode - the net stack isolates their sockets by kobject
 * permission, so the apps are independent. Killing one networked app leaves the
 * other online and resolving.
 */

#include "app_group.h"
#include "svc_broker.h"
#include "svc_registry.h"
#include "net_link.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/socket.h>

/*
 * These networked apps need no private data partition: their isolation that
 * matters is per-socket (the net stack grants each app a kobject permission on
 * its own socket, so apps cannot touch each other's connections). They run in
 * the default domain with stack isolation; pass NULL for the partition.
 */

/* Each app resolves a different host, on its own, to show independence. */
static void net_app(struct app_group *g, void *arg)
{
	ARG_UNUSED(g);
	const char *host = (const char *)arg;

	/* Wait for the link, heartbeating so the watchdog does not reclaim us
	 * while the supervisor is still bringing Wi-Fi/DHCP up.
	 */
	bool ready = false;

	for (int w = 0; w < 120; w++) {
		if (net_link_wait_ready(K_MSEC(250)) == 0) {
			ready = true;
			break;
		}
		svc_heartbeat();
	}
	if (!ready) {
		printk("  [%s-app] network never came up\n", host);
		return;
	}

	for (int i = 0; ; i++) {
		int rc = svc_net_probe(80);

		printk("  [%s-app] #%d net probe -> %s (rc=%d)\n",
		       host, i, rc == 0 ? "connected" : "no-connect", rc);

		svc_heartbeat();
		k_msleep(2000);
	}
}

/* A bad networked app: resolves once, then hangs - must not affect the other. */
static void bad_net_app(struct app_group *g, void *arg)
{
	ARG_UNUSED(g);
	const char *host = (const char *)arg;

	bool ready = false;

	for (int w = 0; w < 120; w++) {
		if (net_link_wait_ready(K_MSEC(250)) == 0) {
			ready = true;
			break;
		}
		svc_heartbeat();
	}
	if (!ready) {
		return;
	}

	int rc = svc_net_probe(80);

	printk("  [%s-BAD] probed once (rc=%d), now HANGING\n", host, rc);
	for (;;) {
		/* stop heartbeating -> supervisor reclaims this app only */
	}
}

/* Grant a group the net-ready semaphore so its app can wait on it. */
extern struct k_sem net_ready_sem;

int main(void)
{
	printk("\n*** M2: two apps, independent internet over shared Wi-Fi (C6) ***\n");

	if (app_group_init() != 0) {
		printk("app_group_init failed\n");
		return 0;
	}

	if (net_link_up() != 0) {
		printk("net_link_up failed\n");
		return 0;
	}

	/* App A: independently resolves example.com forever. */
	struct app_group *a = app_group_create("A", NULL, net_app,
					       (void *)"example.com");
	app_group_grant_service(a, SVC_DEMO);
	k_object_access_grant(&net_ready_sem, &a->thread);
	app_group_start(a);

	/* App B: a bad app that resolves once then hangs. */
	struct app_group *b = app_group_create("B", NULL, bad_net_app,
					       (void *)"zephyrproject.org");
	app_group_grant_service(b, SVC_DEMO);
	k_object_access_grant(&net_ready_sem, &b->thread);
	app_group_start(b);

	printk("M2: apps running; App B will hang and be reclaimed, App A stays online\n");
	return 0;
}
