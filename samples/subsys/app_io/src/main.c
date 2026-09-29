/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application using the platform's general-I/O services:
 * it drives the status LEDs it was granted, reacts to button presses, and keeps
 * a checkpoint value the platform retains across a restart. The LED pins are
 * scoped by a per-application grant, so an ungranted pin is refused at the
 * service boundary. Everything the application touches goes through a
 * capability-gated syscall; it links no driver code.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

static void io_app(void *arg)
{
	ARG_UNUSED(arg);

	uint32_t mark = app_svc_checkpoint_get();

	if (mark == 0) {
		app_svc_log("io: first run, no checkpoint yet");
	} else {
		char line[40];

		(void)snprintk(line, sizeof(line), "io: resumed at checkpoint %u",
			       mark);
		app_svc_log(line);
	}

	/* Resolve LEDs by their devicetree name once, then use the indices. The
	 * app addresses LEDs by a stable name, not a position that could shift.
	 */
	int led_a = app_svc_led_find("status LED 0");
	int led_b = app_svc_led_find("status LED 1");

	if (led_a < 0 || led_b < 0) {
		app_svc_log("io: could not resolve LED names");
		return;
	}
	app_svc_log("io: resolved LEDs by name");
	int pins[2] = { led_a, led_b };

	/* Walk the granted LEDs on a few rounds, reacting to any press. */
	for (int round = 0; round < 5; round++) {
		int pin = pins[round % 2];
		int rc = app_svc_button_wait(1000);

		if (rc >= 0) {
			app_svc_led_set(pin, 1);
			app_svc_log("io: button press -> led on");
		} else {
			app_svc_led_set(pin, round & 1);
			app_svc_log("io: idle round, toggled a granted led");
		}
		app_svc_checkpoint_set((uint32_t)(round + 1));
	}

	/* The third LED, resolved by name, was never granted: the boundary must
	 * refuse it even though the app can name it.
	 */
	int led_c = app_svc_led_find("status LED 2");

	if (led_c >= 0 && app_svc_led_set(led_c, 1) == -EACCES) {
		app_svc_log("io: ungranted led 'status LED 2' correctly denied");
	} else {
		app_svc_log("io: ERROR ungranted led was allowed");
	}

	app_svc_led_set(led_a, 0);
	app_svc_led_set(led_b, 0);
	app_svc_log("io: done");
}

int main(void)
{
	printk("\n*** app_io: led + button + checkpoint via platform services ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "io",
		.source = APP_BUILTIN,
		.entry = io_app,
		.caps = APP_CAP_LOG | APP_CAP_LED | APP_CAP_INPUT,
		.stopped = true,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}

	/* Grant LED pins 0 and 1 to this application; pin 2 stays denied. */
	app_platform_grant_led(id, BIT(0) | BIT(1));
	(void)app_platform_start(id);

	printk("*** launched isolated io app (group %d) ***\n", id);
	return 0;
}
