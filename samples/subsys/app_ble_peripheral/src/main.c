/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application serving its own GATT service, so a phone
 * or any other central can connect to the board and talk to the application.
 *
 * The application declares a service and two characteristics by UUID and the
 * platform does the rest: it registers the service, advertises, owns the
 * connection and answers reads from values the application publishes. A peer's
 * write is delivered as an ordinary input event, dispatched to a handler that
 * runs on the application's own thread in user mode, so nothing calls from the
 * Bluetooth stack into application code.
 *
 * Counter characteristic  a1b2c3a1-...  read: seconds since start
 * Config characteristic   a1b2c3a2-...  write: the application logs what it got
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_dispatch.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <string.h>

#define ADV_NAME  "c5-app"
#define CHR_COUNT 0
#define CHR_CONF  1

/*
 * UUIDs in little-endian byte order, as the service takes them. They are read
 * by the application in user mode, so they live in a partition added to its
 * domain rather than in kernel rodata, which it may not touch.
 */
K_APPMEM_PARTITION_DEFINE(per_part);

K_APP_DMEM(per_part) uint8_t svc_uuid[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x44, 0x44, 0xa0, 0xc3, 0xb2, 0xa1,
};
K_APP_DMEM(per_part) uint8_t count_uuid[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x44, 0x44, 0xa1, 0xc3, 0xb2, 0xa1,
};
K_APP_DMEM(per_part) uint8_t conf_uuid[16] = {
	0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x7d, 0x8c,
	0x5b, 0x4a, 0x44, 0x44, 0xa2, 0xc3, 0xb2, 0xa1,
};

struct state {
	struct app_dispatch d;
	int writes;
};

/* The platform reports a peer arriving and leaving as ordinary events, so the
 * application learns of them on the same queue it already reads.
 */
static void on_connect(const struct app_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
	app_svc_log("peripheral: a peer connected");
}

static void on_disconnect(const struct app_event *evt, void *user)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
	app_svc_log("peripheral: the peer went away, advertising again");
}

static void on_write(const struct app_event *evt, void *user)
{
	struct state *s = user;
	uint8_t buf[16];
	char line[72];

	s->writes++;

	int n = app_svc_ble_serve_get((int)evt->line, buf, sizeof(buf));

	if (n < 0) {
		(void)snprintk(line, sizeof(line), "peripheral: read back failed (%d)", n);
		app_svc_log(line);
		return;
	}
	(void)snprintk(line, sizeof(line),
		       "peripheral: peer wrote chr %u, %d byte(s), first 0x%02x (#%d)",
		       evt->line, n, (n > 0) ? buf[0] : 0, s->writes);
	app_svc_log(line);
}

static void peripheral_app(void *arg)
{
	ARG_UNUSED(arg);

	struct state state = {0};

	struct app_ble_chrc c[2] = {
		[CHR_COUNT] = { .uuid_len = 16, .readable = 1, .writable = 0,
				.notifiable = 1 },
		[CHR_CONF]  = { .uuid_len = 16, .readable = 1, .writable = 1 },
	};

	memcpy(c[CHR_COUNT].uuid, count_uuid, 16);
	memcpy(c[CHR_CONF].uuid, conf_uuid, 16);

	int rc = app_svc_ble_serve_start(ADV_NAME, svc_uuid, sizeof(svc_uuid), c, 2);

	if (rc != 0) {
		char line[48];

		(void)snprintk(line, sizeof(line), "peripheral: serve failed (%d)", rc);
		app_svc_log(line);
		return;
	}
	app_svc_log("peripheral: advertising as '" ADV_NAME "', connect and write");

	(void)app_dispatch_on(&state.d, APP_EVENT_GATT_WRITE, on_write, &state);
	(void)app_dispatch_on(&state.d, APP_EVENT_BLE_CONNECTED, on_connect,
			      &state);
	(void)app_dispatch_on(&state.d, APP_EVENT_BLE_DISCONNECTED,
			      on_disconnect, &state);

	/* Publish an increasing value so a read shows something live, and
	 * dispatch any writes that arrive in between. A peer that subscribed
	 * is pushed the value instead of having to poll for it; without a
	 * subscriber the notify is refused and the stored value still serves
	 * a plain read.
	 */
	for (uint32_t sec = 0; ; sec++) {
		uint8_t v[4] = {
			(uint8_t)sec, (uint8_t)(sec >> 8), 0, 0,
		};

		if (app_svc_ble_notify(CHR_COUNT, v, sizeof(v)) != 0) {
			(void)app_svc_ble_serve_set(CHR_COUNT, v, sizeof(v));
		}
		(void)app_dispatch_run(&state.d, 1000);
	}
}

int main(void)
{
	printk("\n*** app_ble_peripheral: an application serving GATT ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "peripheral",
		.source = APP_BUILTIN,
		.entry = peripheral_app,
		.caps = APP_CAP_LOG | APP_CAP_BLE | APP_CAP_INPUT,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}
	app_platform_grant_ble_gatt(id, svc_uuid, sizeof(svc_uuid));
	k_mem_domain_add_partition(app_platform_domain(id), &per_part);

	printk("*** launched isolated peripheral app (group %d) ***\n", id);
	return 0;
}
