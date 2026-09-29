/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application using the platform's CAN service. The
 * platform owns the controller and runs it in loopback, so the frames this
 * application sends come back to it on a single board with no transceiver
 * wired up. The application links no driver code and never holds the bus: it
 * hands payload bytes to a capability-gated syscall and reads frames back from
 * a queue the platform fills. A second application launched without
 * APP_CAP_CAN shows the service refusing an ungranted caller.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

#define CAN_ID_BASE 0x123U
#define ROUNDS      4

static void can_app(void *arg)
{
	ARG_UNUSED(arg);

	app_svc_log("can: sending frames in loopback");

	for (int round = 0; round < ROUNDS; round++) {
		uint8_t tx[4] = { (uint8_t)round, 0xAA, 0x55, (uint8_t)(round + 1) };
		uint32_t id = CAN_ID_BASE + (uint32_t)round;
		int rc = app_svc_can_send(id, tx, sizeof(tx));

		if (rc != 0) {
			char line[48];

			(void)snprintk(line, sizeof(line), "can: send failed (%d)", rc);
			app_svc_log(line);
			return;
		}

		uint32_t got_id = 0;
		uint8_t rx[8];
		int n = app_svc_can_recv(&got_id, rx, sizeof(rx), 500);

		if (n < 0) {
			char line[48];

			(void)snprintk(line, sizeof(line), "can: recv failed (%d)", n);
			app_svc_log(line);
			return;
		}

		bool match = (got_id == id) && (n == (int)sizeof(tx)) &&
			     (rx[0] == tx[0]) && (rx[1] == tx[1]) &&
			     (rx[2] == tx[2]) && (rx[3] == tx[3]);

		char line[64];

		(void)snprintk(line, sizeof(line),
			       "can: round %d id 0x%x len %d %s", round, got_id, n,
			       match ? "matches" : "MISMATCH");
		app_svc_log(line);

		if (!match) {
			return;
		}
	}

	/* A payload longer than a classic CAN frame must be refused rather than
	 * silently truncated.
	 */
	uint8_t big[9] = {0};

	if (app_svc_can_send(CAN_ID_BASE, big, sizeof(big)) == -EINVAL) {
		app_svc_log("can: oversized payload correctly refused");
	} else {
		app_svc_log("can: ERROR oversized payload was accepted");
	}

	app_svc_log("can: done");
}

static void nocan_app(void *arg)
{
	ARG_UNUSED(arg);

	uint8_t tx[2] = { 0x01, 0x02 };

	if (app_svc_can_send(CAN_ID_BASE, tx, sizeof(tx)) == -EPERM) {
		app_svc_log("nocan: send denied without the capability");
	} else {
		app_svc_log("nocan: ERROR send was allowed");
	}

	uint32_t id = 0;
	uint8_t rx[8];

	if (app_svc_can_recv(&id, rx, sizeof(rx), 100) == -EPERM) {
		app_svc_log("nocan: recv denied without the capability");
	} else {
		app_svc_log("nocan: ERROR recv was allowed");
	}
}

int main(void)
{
	printk("\n*** app_can: CAN frames through a platform service ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "can",
		.source = APP_BUILTIN,
		.entry = can_app,
		.caps = APP_CAP_LOG | APP_CAP_CAN,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}

	struct app_def denied = {
		.name = "nocan",
		.source = APP_BUILTIN,
		.entry = nocan_app,
		.caps = APP_CAP_LOG,
	};

	if (app_platform_launch(&denied) < 0) {
		printk("ungranted launch failed\n");
		return 0;
	}

	printk("*** launched isolated can app (group %d) ***\n", id);
	return 0;
}
