/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * V4 spike: prove Wi-Fi coexists with USERSPACE/PMP on C6 when driven through a
 * broker. A user-mode app calls svc_wifi_scan (gated on a service token); the
 * scan runs in supervisor mode via net_mgmt. The supervisor counts scan
 * results via a net_mgmt event callback. No AP/credentials needed.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fatal.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>

#define STACK_SIZE 4096
#define APP_PRIO   5

K_SEM_DEFINE(svc_token, 0, 1);

K_THREAD_STACK_DEFINE(stack_a, STACK_SIZE);
static struct k_thread th_a;

static volatile bool app_started;
static volatile int  scan_results;
static volatile bool scan_done;

static K_SEM_DEFINE(a_done, 0, 1);

static struct net_mgmt_event_callback wifi_cb;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_KERNEL_PANIC) {
		printk("PANIC, halting\n");
		k_fatal_halt(reason);
	}
	printk("  [fault] reason %u in thread %p (V4 unexpected)\n",
	       reason, k_current_get());
}

static void wifi_evt(struct net_mgmt_event_callback *cb,
		     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_SCAN_RESULT:
		scan_results++;
		break;
	case NET_EVENT_WIFI_SCAN_DONE:
		scan_done = true;
		printk("supervisor: scan DONE, %d result(s)\n", scan_results);
		break;
	default:
		break;
	}
}

/* User-mode app: trigger the brokered scan. */
static void app_a_entry(void *token, void *p2, void *p3)
{
	ARG_UNUSED(p2); ARG_UNUSED(p3);

	int rc = svc_wifi_scan((struct k_sem *)token);

	printk("App A (user mode): svc_wifi_scan -> rc=%d\n", rc);
	k_sem_give(&a_done);
	for (;;) {
		k_msleep(100);
	}
}

int main(void)
{
	k_tid_t t;

	printk("\n=== V4: brokered Wi-Fi scan under USERSPACE (C6) ===\n");

	net_mgmt_init_event_callback(&wifi_cb, wifi_evt,
				     NET_EVENT_WIFI_SCAN_RESULT |
				     NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&wifi_cb);

	t = k_thread_create(&th_a, stack_a, STACK_SIZE, app_a_entry,
			    &svc_token, NULL, NULL, APP_PRIO, K_USER, K_FOREVER);
	k_thread_access_grant(t, &a_done);
	k_object_access_grant(&svc_token, t);
	app_started = true;
	k_thread_start(t);

	/* Wait for the app's syscall to return. */
	bool app_ok = (k_sem_take(&a_done, K_SECONDS(5)) == 0);

	/* Give the scan time to complete and emit results. */
	for (int i = 0; i < 100 && !scan_done; i++) {
		k_msleep(100);
	}

	printk("\n=== V4 RESULT ===\n");
	printk("  user app reached broker syscall: %s\n", app_ok ? "OK" : "FAIL");
	printk("  Wi-Fi scan ran under USERSPACE: %s\n",
	       scan_done ? "OK" : "FAIL (no scan-done)");
	printk("  scan results seen: %d\n", scan_results);
	printk("  V4 %s\n", (app_ok && scan_done) ? "PASS" : "FAIL");
	return 0;
}
