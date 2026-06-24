/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "policy.h"
#include "cap_surface.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(behavior_sandbox, CONFIG_LOG_DEFAULT_LEVEL);

static void policy_good(void)
{
	for (int i = 0; i < 5; i++) {
		int32_t milli_c = 0;
		int ret;

		(void)cap_led_set(true);
		cap_sleep(200);
		(void)cap_led_set(false);

		ret = cap_sensor_read(&milli_c);
		if (ret == 0) {
			LOG_INF("policy: die temp %d.%03d C",
				milli_c / 1000, milli_c % 1000);
		} else {
			LOG_WRN("policy: sensor read failed: %d", ret);
		}

		cap_sleep(800);
	}

	LOG_INF("policy: good behavior finished cleanly");
}

static void policy_rogue(void)
{
	/*
	 * The sandbox has no business touching arbitrary memory. This is the
	 * kind of thing a compromised or hallucinated payload would attempt.
	 * Under USERSPACE the PMP must trap this and abort only this thread,
	 * leaving the supervisor and the rest of the system alive.
	 */
	volatile uint32_t *forbidden = (volatile uint32_t *)0x60000000;

	LOG_WRN("policy: attempting raw access outside the capability gate");
	*forbidden = 0xdeadbeef;

	/* Must never be reached. */
	LOG_ERR("policy: containment FAILED, raw write succeeded");
}

static void policy_runaway(void)
{
	LOG_WRN("policy: entering runaway loop, watchdog must intervene");
	for (;;) {
		/* No capability calls, no yields: starve nothing but burn
		 * the watchdog budget so the supervisor must kill us.
		 */
	}
}

void policy_run(void *kind, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);

	switch ((enum policy_kind)(uintptr_t)kind) {
	case POLICY_GOOD:
		policy_good();
		break;
	case POLICY_ROGUE:
		policy_rogue();
		break;
	case POLICY_RUNAWAY:
		policy_runaway();
		break;
	default:
		LOG_ERR("policy: unknown kind");
		break;
	}
}
