/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application that reacts to input through registered
 * callbacks instead of a blocking wait per source. The handlers run on the
 * application's own thread, in user mode and in its own memory domain: the
 * dispatcher is a loop around app_svc_event_wait(), so nothing calls from the
 * platform into the application and a faulting handler is an ordinary
 * application fault.
 *
 * Two sources are dispatched from the one loop, which is what a per-source
 * blocking call cannot do: the board button, and level changes on a granted
 * GPIO line. Press the board button to drive the first; wire an edge onto the
 * granted line to drive the second.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_dispatch.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <errno.h>

#define WATCH_LINE 0
#define IDLE_MS    30000

/* Kept together so a handler can reach the table it was dispatched from. */
struct holder {
	struct app_dispatch d;
	int                 button;
	int                 gpio;
};

/* The dispatch table and its counters are touched by the application in user
 * mode, so they live in a partition added to its domain rather than in plain
 * kernel .bss, which the application may not read.
 */
K_APPMEM_PARTITION_DEFINE(disp_part);
K_APP_DMEM(disp_part) struct holder holder;

static void on_button(const struct app_event *evt, void *user)
{
	struct holder *h = user;
	char line[56];

	h->button++;
	(void)snprintk(line, sizeof(line),
		       "dispatch: key 0x%x #%d (seq %u)", evt->code, h->button,
		       evt->seq);
	app_svc_log(line);
}

static void on_gpio(const struct app_event *evt, void *user)
{
	struct holder *h = user;
	char line[64];

	h->gpio++;
	(void)snprintk(line, sizeof(line),
		       "dispatch: gpio line %u -> %u (#%d, seq %u)",
		       evt->line, evt->value, h->gpio, evt->seq);
	app_svc_log(line);

	if (h->gpio >= 4) {
		app_svc_log("dispatch: seen enough, stopping");
		app_dispatch_stop(&h->d);
	}
}

static void dispatch_app(void *arg)
{
	ARG_UNUSED(arg);

	if (app_svc_gpio_watch(WATCH_LINE, 1) != 0) {
		app_svc_log("dispatch: could not watch the granted line");
		return;
	}
	app_svc_log("dispatch: watching a granted line and the button");

	(void)app_dispatch_on(&holder.d, APP_EVENT_BUTTON, on_button, &holder);
	(void)app_dispatch_on(&holder.d, APP_EVENT_GPIO, on_gpio, &holder);

	int rc = app_dispatch_run(&holder.d, IDLE_MS);
	char line[64];

	(void)snprintk(line, sizeof(line),
		       "dispatch: loop ended rc=%d button=%d gpio=%d", rc,
		       holder.button, holder.gpio);
	app_svc_log(line);
}

static void nodispatch_app(void *arg)
{
	ARG_UNUSED(arg);

	struct app_event evt;

	if (app_svc_event_wait(&evt, 100) == -EPERM) {
		app_svc_log("nodispatch: wait denied without the capability");
	} else {
		app_svc_log("nodispatch: ERROR wait was allowed");
	}
}

int main(void)
{
	printk("\n*** app_dispatch: input events as callbacks ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "dispatch",
		.source = APP_BUILTIN,
		.entry = dispatch_app,
		.caps = APP_CAP_LOG | APP_CAP_INPUT | APP_CAP_GPIO,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}
	app_platform_grant_gpio(id, BIT(WATCH_LINE));
	k_mem_domain_add_partition(app_platform_domain(id), &disp_part);

	struct app_def denied = {
		.name = "nodispatch",
		.source = APP_BUILTIN,
		.entry = nodispatch_app,
		.caps = APP_CAP_LOG,
	};

	(void)app_platform_launch(&denied);
	printk("*** launched isolated dispatch app (group %d) ***\n", id);

	printk("*** press the board button to drive the dispatcher ***\n");
	return 0;
}
