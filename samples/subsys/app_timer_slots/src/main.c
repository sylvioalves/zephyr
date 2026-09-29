/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application running several timers at once. It claims
 * two of its timer slots, arms them for different intervals and waits on each
 * separately, then checks the periodic mode, the elapsed count and that a
 * released slot returns to the application's pool. Each slot is a platform
 * object reached through a capability-gated syscall; the application owns no
 * kernel timer.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

/* Counters an application writes in user mode live in a partition added to its
 * domain; ordinary globals are not covered by it.
 */
K_APPMEM_PARTITION_DEFINE(timer_part);
K_APP_DMEM(timer_part) static int passes;
K_APP_DMEM(timer_part) static int fails;

static void check(bool ok, const char *what, int rc)
{
	char line[72];

	if (ok) {
		(void)snprintk(line, sizeof(line), "PASS %s", what);
		passes++;
	} else {
		(void)snprintk(line, sizeof(line), "FAIL %s (rc=%d)", what, rc);
		fails++;
	}
	app_svc_log(line);
}

static void timer_app(void *arg)
{
	ARG_UNUSED(arg);

	app_svc_log("timer_slots: start");

	int a = app_svc_timer_create();
	int b = app_svc_timer_create();

	check(a >= 0, "claim a slot", a);
	check(b >= 0 && b != a, "claim a second, distinct slot", b);

	/* The point of slots: two timers run at once and each is waited on
	 * separately, which a single per-application timer could not do.
	 */
	int rc = app_svc_timer_arm(a, 120, 0);

	check(rc == 0, "arm the first for 120ms", rc);
	rc = app_svc_timer_arm(b, 40, 0);
	check(rc == 0, "arm the second for 40ms", rc);

	rc = app_svc_timer_slot_wait(b, 500);
	check(rc == 0, "the shorter one fires first", rc);

	rc = app_svc_timer_slot_wait(a, 0);
	check(rc == -EAGAIN, "the longer one is still pending", rc);

	rc = app_svc_timer_slot_wait(a, 500);
	check(rc == 0, "the longer one fires after it", rc);

	int el = app_svc_timer_elapsed(a);

	check(el >= 100 && el < 400, "elapsed is in range", el);

	rc = app_svc_timer_arm(a, 50, 1);
	check(rc == 0, "arm periodic", rc);
	rc = app_svc_timer_slot_wait(a, 400);
	check(rc == 0, "periodic fires", rc);
	rc = app_svc_timer_slot_wait(a, 400);
	check(rc == 0, "periodic fires again without re-arming", rc);

	rc = app_svc_timer_stop(a);
	check(rc == 0, "stop it", rc);
	rc = app_svc_timer_slot_wait(a, 150);
	check(rc == -EAGAIN, "a stopped slot does not fire", rc);

	rc = app_svc_timer_arm(99, 10, 0);
	check(rc == -EINVAL, "an out-of-range slot is refused", rc);

	rc = app_svc_timer_free(a);
	check(rc == 0, "release the first slot", rc);
	rc = app_svc_timer_arm(a, 10, 0);
	check(rc == -EINVAL, "a released slot is refused until claimed again",
	      rc);
	rc = app_svc_timer_free(b);
	check(rc == 0, "release the second slot", rc);

	/* Every slot was claimed and released, so the pool must come back
	 * whole and then report exhaustion rather than handing out more.
	 */
	int got = 0;

	for (int i = 0; i < APP_TIMER_SLOTS; i++) {
		if (app_svc_timer_create() >= 0) {
			got++;
		}
	}
	check(got == APP_TIMER_SLOTS, "the whole pool is claimable again", got);

	rc = app_svc_timer_create();
	check(rc == -ENOSPC, "an exhausted pool is refused", rc);

	for (int i = 0; i < APP_TIMER_SLOTS; i++) {
		(void)app_svc_timer_free(i);
	}

	char line[48];

	(void)snprintk(line, sizeof(line), "timer_slots: %d passed, %d failed",
		       passes, fails);
	app_svc_log(line);
	app_svc_log(fails == 0 ? "timer_slots: done" : "timer_slots: FAILURES");
}

int main(void)
{
	printk("\n*** app_timer_slots: several timers in one application ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "timers",
		.source = APP_BUILTIN,
		.entry = timer_app,
		.caps = APP_CAP_LOG | APP_CAP_TIME,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}
	k_mem_domain_add_partition(app_platform_domain(id), &timer_part);

	printk("*** launched isolated timer app (group %d) ***\n", id);
	return 0;
}
