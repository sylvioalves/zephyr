/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The bulk calls: several GPIO lines driven in one call and several sensor
 * samples read in one call, checked against emulated devices and timed against
 * the same work done one call at a time.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/emul_sensor.h>
#include <errno.h>

#include "throughput.h"

#define PLATFORM_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_app_platform)
#define LINE_COUNT    DT_PROP_LEN(PLATFORM_NODE, gpios)
#define ALL_LINES     ((uint32_t)BIT64_MASK(LINE_COUNT))
#define SENSOR_IDX    0
#define BULK_CAPS     (APP_CAP_LOG | APP_CAP_TIME | APP_CAP_GPIO | APP_CAP_SENSOR)
#define REPS          3

BUILD_ASSERT(LINE_COUNT == 32, "the suite drives 32 lines");

#define LINE_SPEC(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

static const struct gpio_dt_spec lines[] = {
	DT_FOREACH_PROP_ELEM(PLATFORM_NODE, gpios, LINE_SPEC)
};

static const struct emul *const sensor_emul = EMUL_DT_GET(DT_NODELABEL(bulk_sensor));

K_APP_DMEM(report_part) volatile int bulk_rc = -9999;
K_APP_DMEM(report_part) volatile int bulk_rc2 = -9999;
K_APP_DMEM(report_part) volatile int bulk_done;
K_APP_DMEM(report_part) volatile int bulk_holding;
K_APP_DMEM(report_part) volatile int bulk_release;
K_APP_DMEM(report_part) volatile int bulk_cmd;
K_APP_DMEM(report_part) volatile int bulk_ack;
K_APP_DMEM(report_part) volatile uint32_t bulk_arg_mask;
K_APP_DMEM(report_part) volatile uint32_t bulk_arg_values;
K_APP_DMEM(report_part) int32_t samples[CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX + 1];

/* Cycles of N single calls and of one bulk call, for N = 8 and N = 32. */
K_APP_DMEM(report_part) uint32_t gpio_single[2];
K_APP_DMEM(report_part) uint32_t gpio_bulk[2];
K_APP_DMEM(report_part) uint32_t sensor_single[2];
K_APP_DMEM(report_part) uint32_t sensor_bulk[2];

static const uint32_t bench_n[2] = { 8U, 32U };

/* Logical level of a line, from the level the emulated port drives. */
static int line_level(int i)
{
	int raw = gpio_emul_output_get(lines[i].port, lines[i].pin);

	return ((lines[i].dt_flags & GPIO_ACTIVE_LOW) != 0U) ? !raw : raw;
}

static uint32_t levels(void)
{
	uint32_t v = 0U;

	for (int i = 0; i < LINE_COUNT; i++) {
		if (line_level(i) > 0) {
			v |= BIT(i);
		}
	}
	return v;
}

/* Lines granted to the next application; a reclaim drops the grants of a slot. */
static uint32_t granted;

static void grant_lines(uint32_t mask)
{
	granted = mask;
}

static int launch(const char *name, app_entry_t entry)
{
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_gpio(i, granted);
	}
	return tp_launch(name, entry, BULK_CAPS);
}

static void set_temp(q31_t q)
{
	struct sensor_chan_spec ch = { .chan_type = SENSOR_CHAN_DIE_TEMP, .chan_idx = 0 };

	zassert_ok(emul_sensor_backend_set_channel(sensor_emul, ch, &q, 5));
}

static void report(int rc)
{
	bulk_rc = rc;
	bulk_done = 1;
}

static void run_app(const char *name, app_entry_t entry)
{
	bulk_rc = -9999;
	bulk_rc2 = -9999;
	bulk_done = 0;
	(void)launch(name, entry);
	zassert_true(tp_wait_gone(name, 5000), "%s never exited", name);
	zassert_equal(bulk_done, 1, "%s did not report", name);
}

/* Drive lines on request until released, so the levels can be checked while
 * the application still owns them: a reclaim drives its active lines low.
 */
static void set_mask_server_app(void *arg)
{
	int seen = 0;

	ARG_UNUSED(arg);

	bulk_holding = 1;
	while (bulk_release == 0) {
		if (bulk_cmd != seen) {
			seen = bulk_cmd;
			bulk_rc = app_svc_gpio_set_mask(bulk_arg_mask, bulk_arg_values);
			bulk_ack = seen;
		}
		k_msleep(1);
	}
	bulk_done = 1;
}

static void set_mask_held_app(void *arg)
{
	ARG_UNUSED(arg);

	report(app_svc_gpio_set_mask(bulk_arg_mask, bulk_arg_values));
	bulk_holding = 1;
	while (bulk_release == 0) {
		k_msleep(1);
	}
}

static void server_start(void)
{
	bulk_cmd = 0;
	bulk_ack = 0;
	bulk_done = 0;
	bulk_holding = 0;
	bulk_release = 0;
	(void)launch("gpio_server", set_mask_server_app);
	for (int i = 0; i < 400 && bulk_holding == 0; i++) {
		k_msleep(5);
	}
	zassert_equal(bulk_holding, 1, "gpio_server did not start");
}

static void server_stop(void)
{
	bulk_release = 1;
	zassert_true(tp_wait_gone("gpio_server", 2000), "gpio_server never exited");
	zassert_equal(bulk_done, 1, "gpio_server did not end on its own");
}

static int drive(uint32_t mask, uint32_t values)
{
	bulk_arg_mask = mask;
	bulk_arg_values = values;
	bulk_cmd++;
	for (int i = 0; i < 400 && bulk_ack != bulk_cmd; i++) {
		k_msleep(5);
	}
	zassert_equal(bulk_ack, bulk_cmd, "gpio_server did not answer");
	return bulk_rc;
}

ZTEST(bulk, test_gpio_set_mask_drives_lines)
{
	grant_lines(ALL_LINES);
	server_start();
	zassert_equal(levels(), 0U, "lines not all low: %08x", levels());

	zassert_ok(drive(ALL_LINES, 0xA5C3F00FU));
	zassert_equal(levels(), 0xA5C3F00FU, "levels %08x", levels());

	zassert_ok(drive(0x00FF00FFU, 0x0F0F0F0FU));
	zassert_equal(levels(), 0xA50FF00FU, "levels %08x", levels());

	zassert_ok(drive(0U, ALL_LINES), "an empty mask was refused");
	zassert_equal(levels(), 0xA50FF00FU, "an empty mask changed a line");

	server_stop();
	zassert_equal(levels(), 0U, "exit left lines active: %08x", levels());
}

ZTEST(bulk, test_gpio_set_mask_needs_every_grant)
{
	grant_lines(0x0000000FU);
	server_start();
	zassert_equal(drive(0x0000001FU, 0x0000001FU), -EACCES);
	zassert_equal(levels(), 0U, "a refused call drove a line");
	zassert_equal(drive(BIT(31), BIT(31)), -EACCES);
	zassert_equal(levels(), 0U, "a refused call drove a line");
	zassert_ok(drive(0x0000000FU, 0x0000000AU));
	zassert_equal(levels(), 0x0000000AU, "levels %08x", levels());
	server_stop();
}

ZTEST(bulk, test_gpio_set_mask_idle_on_reclaim)
{
	int id;

	grant_lines(ALL_LINES);
	bulk_arg_mask = 0x80000101U;
	bulk_arg_values = 0x80000101U;
	bulk_holding = 0;
	bulk_release = 0;
	id = launch("gpio_held", set_mask_held_app);
	for (int i = 0; i < 400 && bulk_holding == 0; i++) {
		k_msleep(5);
	}
	zassert_equal(bulk_holding, 1, "gpio_held never reported");
	zassert_ok(bulk_rc);
	zassert_equal(levels(), 0x80000101U, "levels %08x", levels());

	zassert_ok(app_platform_remove(id));
	zassert_true(tp_wait_gone("gpio_held", 2000), "gpio_held never reclaimed");
	zassert_equal(levels(), 0U, "reclaim left lines active: %08x", levels());
	bulk_release = 1;
}

static void read_n_app(void *arg)
{
	ARG_UNUSED(arg);

	bulk_rc2 = app_svc_sensor_read_n(SENSOR_IDX, samples, 0U);
	if (app_svc_sensor_read_n(SENSOR_IDX, samples,
				  CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX + 1U) != -EINVAL) {
		bulk_rc2 = -EIO;
	}
	report(app_svc_sensor_read_n(SENSOR_IDX, samples, CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX));
}

ZTEST(bulk, test_sensor_read_n)
{
	/* 20.0 degrees as a q31 value with a shift of 5. */
	set_temp(1342177280);
	run_app("read_n", read_n_app);
	zassert_equal(bulk_rc2, -EINVAL, "a count of 0 or above the maximum was taken");
	zassert_equal(bulk_rc, CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX, "read_n returned %d",
		      bulk_rc);
	for (int i = 0; i < CONFIG_APP_PLATFORM_SENSOR_READ_N_MAX; i++) {
		zassert_within(samples[i], 20000, 1, "sample %d is %d", i, samples[i]);
	}
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return (a < b) ? a : b;
}

static void gpio_bench_app(void *arg)
{
	int rc = 0;

	ARG_UNUSED(arg);

	for (int k = 0; k < 2; k++) {
		uint32_t n = bench_n[k];
		uint32_t mask = (uint32_t)BIT64_MASK(n);

		gpio_single[k] = UINT32_MAX;
		gpio_bulk[k] = UINT32_MAX;
		for (int r = 0; r < REPS && rc == 0; r++) {
			uint32_t t0 = app_svc_cycles();

			for (uint32_t i = 0; i < n && rc == 0; i++) {
				rc = app_svc_gpio_set((int)i, (int)(r & 1));
			}
			gpio_single[k] = min_u32(gpio_single[k], app_svc_cycles() - t0);

			t0 = app_svc_cycles();
			if (rc == 0) {
				rc = app_svc_gpio_set_mask(mask, ((r & 1) != 0) ? 0U : mask);
			}
			gpio_bulk[k] = min_u32(gpio_bulk[k], app_svc_cycles() - t0);
		}
	}
	(void)app_svc_gpio_set_mask(ALL_LINES, 0U);
	report(rc);
}

static void sensor_bench_app(void *arg)
{
	int rc = 0;

	ARG_UNUSED(arg);

	for (int k = 0; k < 2; k++) {
		uint32_t n = bench_n[k];

		sensor_single[k] = UINT32_MAX;
		sensor_bulk[k] = UINT32_MAX;
		for (int r = 0; r < REPS && rc == 0; r++) {
			uint32_t t0 = app_svc_cycles();

			for (uint32_t i = 0; i < n && rc == 0; i++) {
				rc = app_svc_sensor_read(SENSOR_IDX, &samples[i]);
			}
			sensor_single[k] = min_u32(sensor_single[k], app_svc_cycles() - t0);

			t0 = app_svc_cycles();
			if (rc == 0) {
				rc = app_svc_sensor_read_n(SENSOR_IDX, samples, n);
				rc = (rc == (int)n) ? 0 : -EIO;
			}
			sensor_bulk[k] = min_u32(sensor_bulk[k], app_svc_cycles() - t0);
		}
	}
	report(rc);
}

ZTEST(bulk, test_bulk_calls_cost_less)
{
	set_temp(1342177280);
	grant_lines(ALL_LINES);
	run_app("gpio_bench", gpio_bench_app);
	zassert_ok(bulk_rc, "gpio bench failed: %d", bulk_rc);
	zassert_equal(levels(), 0U, "gpio_bench left lines active: %08x", levels());
	run_app("sensor_bench", sensor_bench_app);
	zassert_ok(bulk_rc, "sensor bench failed: %d", bulk_rc);

	for (int k = 0; k < 2; k++) {
		TC_PRINT("gpio   N=%2u: %u single calls %u cycles, one bulk call %u cycles\n",
			 bench_n[k], bench_n[k], gpio_single[k], gpio_bulk[k]);
		TC_PRINT("sensor N=%2u: %u single calls %u cycles, one bulk call %u cycles\n",
			 bench_n[k], bench_n[k], sensor_single[k], sensor_bulk[k]);
		zassert_true(gpio_bulk[k] < gpio_single[k], "gpio bulk call not cheaper");
		zassert_true(sensor_bulk[k] < sensor_single[k], "sensor bulk call not cheaper");
	}
}
