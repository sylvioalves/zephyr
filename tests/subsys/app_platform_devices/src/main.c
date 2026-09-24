/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The index-keyed device services, driven against emulated devices. What is
 * under test is the platform's own behaviour rather than the drivers': that a
 * service refuses an application without the capability, refuses an instance
 * that was not granted to it, and confines an EEPROM window to the bytes it
 * was given.
 */

#include <zephyr/ztest.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/drivers/dac/dac_emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <string.h>
#include <errno.h>

/* The fake regulator and stepper drivers are FFF-based, so the test image owns
 * the framework's globals.
 */
DEFINE_FFF_GLOBALS;

DECLARE_FAKE_VALUE_FUNC(int, fake_stepper_ctrl_stop, const struct device *);

K_APPMEM_PARTITION_DEFINE(report_part);

K_APP_DMEM(report_part) volatile int report_rc;
K_APP_DMEM(report_part) volatile int report_done;

static int slot_of(const char *name)
{
	for (int i = 0; i < app_platform_count(); i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0 && st.name != NULL &&
		    strcmp(st.name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool wait_slot_free(const char *name, int ms)
{
	for (int i = 0; i < ms / 5; i++) {
		if (slot_of(name) < 0) {
			return true;
		}
		k_msleep(5);
	}
	return slot_of(name) < 0;
}

/* Run an application and require it to report a particular value, rather than
 * to trap: a refusal must come back as an error code the application can see.
 */
static void expect_rc(const char *name, app_entry_t entry, app_caps_t caps,
		      int expected_rc)
{
	const struct app_def def = {
		.name = name, .source = APP_BUILTIN, .entry = entry, .caps = caps,
	};

	report_rc = -9999;
	report_done = 0;

	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "%s launch failed", name);
	k_mem_domain_add_partition(app_platform_domain(id), &report_part);
	zassert_true(wait_slot_free(name, 2000), "%s never exited", name);
	zassert_equal(report_done, 1, "%s trapped instead of reporting", name);
	zassert_equal(report_rc, expected_rc, "%s rc %d != %d", name, report_rc,
		      expected_rc);
}

/* ---- EEPROM window ---- */

static void eeprom_no_window(void *a)
{
	ARG_UNUSED(a);

	uint8_t buf[4];

	report_rc = svc_eeprom_read(0, buf, sizeof(buf));
	report_done = 1;
}

static void eeprom_ungranted(void *a)
{
	ARG_UNUSED(a);

	uint8_t buf[4];

	report_rc = svc_eeprom_read(0, buf, sizeof(buf));
	report_done = 1;
}

/* The window is 64 bytes, so a read that starts inside it but runs past the end
 * must be refused rather than clamped into the neighbour's bytes.
 */
static void eeprom_overrun(void *a)
{
	ARG_UNUSED(a);

	uint8_t buf[16];

	report_rc = svc_eeprom_read(56, buf, sizeof(buf));
	report_done = 1;
}

/* An offset near the top of the range must not wrap past the window end. */
static void eeprom_wrap(void *a)
{
	ARG_UNUSED(a);

	uint8_t buf[8];

	report_rc = svc_eeprom_read(0xffffffffU, buf, sizeof(buf));
	report_done = 1;
}

static void eeprom_roundtrip(void *a)
{
	ARG_UNUSED(a);

	const uint8_t out[4] = {0xde, 0xad, 0xbe, 0xef};
	uint8_t in[4] = {0};
	int rc = svc_eeprom_write(8, out, sizeof(out));

	if (rc != (int)sizeof(out)) {
		report_rc = rc < 0 ? rc : -EIO;
		report_done = 1;
		return;
	}
	rc = svc_eeprom_read(8, in, sizeof(in));
	if (rc != (int)sizeof(in)) {
		report_rc = rc < 0 ? rc : -EIO;
	} else {
		report_rc = memcmp(in, out, sizeof(out)) == 0 ? 0 : -EIO;
	}
	report_done = 1;
}

ZTEST(devices, test_eeprom_gate_and_window)
{
	/* No capability at all. */
	expect_rc("e_np", eeprom_ungranted, APP_CAP_LOG, -EPERM);

	/* Capability but no window assigned. */
	expect_rc("e_nw", eeprom_no_window, APP_CAP_LOG | APP_CAP_EEPROM, -EACCES);

	/* A 64-byte window from offset 128. A reclaim clears the grant, so it is
	 * re-issued before each launch rather than once for the whole test.
	 */
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_eeprom(i, 64, 64);
	}
	expect_rc("e_ov", eeprom_overrun, APP_CAP_LOG | APP_CAP_EEPROM, -EINVAL);

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_eeprom(i, 64, 64);
	}
	expect_rc("e_wr", eeprom_wrap, APP_CAP_LOG | APP_CAP_EEPROM, -EINVAL);

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_eeprom(i, 64, 64);
	}
	expect_rc("e_rt", eeprom_roundtrip, APP_CAP_LOG | APP_CAP_EEPROM, 0);
}

/* Two applications with different windows must not see each other's bytes. */
K_APP_DMEM(report_part) volatile int phase;

static void eeprom_writer(void *a)
{
	ARG_UNUSED(a);

	const uint8_t mark[2] = {0xa5, 0x5a};

	report_rc = svc_eeprom_write(0, mark, sizeof(mark));
	report_done = 1;
}

static void eeprom_peeker(void *a)
{
	ARG_UNUSED(a);

	uint8_t in[2] = {0};
	int rc = svc_eeprom_read(0, in, sizeof(in));

	if (rc != (int)sizeof(in)) {
		report_rc = rc < 0 ? rc : -EIO;
	} else {
		/* Must NOT observe the other application's marker. */
		report_rc = (in[0] == 0xa5 && in[1] == 0x5a) ? -EACCES : 0;
	}
	report_done = 1;
}

ZTEST(devices, test_eeprom_windows_are_private)
{
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_eeprom(i, 0, 32);
	}
	expect_rc("e_w1", eeprom_writer, APP_CAP_LOG | APP_CAP_EEPROM, 2);

	/* Move every slot to a different window and read the same offset. */
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_eeprom(i, 160, 32);
	}
	expect_rc("e_w2", eeprom_peeker, APP_CAP_LOG | APP_CAP_EEPROM, 0);
}

/* ---- DAC ---- */

static void dac_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_dac_count();
	report_done = 1;
}

static void dac_instance_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_dac_write(0, 128);
	report_done = 1;
}

static void dac_write_ok(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_dac_write(0, 128);
	report_done = 1;
}

ZTEST(devices, test_dac_gate_and_grant)
{
	expect_rc("d_np", dac_ungranted, APP_CAP_LOG, -EPERM);
	expect_rc("d_ng", dac_instance_ungranted, APP_CAP_LOG | APP_CAP_DAC, -EACCES);

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_dac(i, BIT(0));
	}
	expect_rc("d_ok", dac_write_ok, APP_CAP_LOG | APP_CAP_DAC, 0);
}

/* ---- real-time clock ---- */

static void rtc_read_ungranted(void *a)
{
	ARG_UNUSED(a);

	struct svc_rtc_time t;

	report_rc = svc_rtcdev_get(&t);
	report_done = 1;
}

/* Reading the clock must not carry the right to set it. */
static void rtc_set_without_set_cap(void *a)
{
	ARG_UNUSED(a);

	const struct svc_rtc_time t = {
		.year = 2026, .month = 9, .day = 8,
		.hour = 12, .minute = 0, .second = 0,
	};

	report_rc = svc_rtcdev_set(&t);
	report_done = 1;
}

static void rtc_roundtrip(void *a)
{
	ARG_UNUSED(a);

	const struct svc_rtc_time set = {
		.year = 2026, .month = 9, .day = 8,
		.hour = 12, .minute = 34, .second = 56,
	};
	struct svc_rtc_time got = {0};
	int rc = svc_rtcdev_set(&set);

	if (rc != 0) {
		report_rc = rc;
		report_done = 1;
		return;
	}
	rc = svc_rtcdev_get(&got);
	if (rc == 0 && (got.year != set.year || got.month != set.month ||
			got.day != set.day || got.hour != set.hour ||
			got.minute != set.minute)) {
		rc = -EIO;
	}
	report_rc = rc;
	report_done = 1;
}

static void rtc_bad_field(void *a)
{
	ARG_UNUSED(a);

	const struct svc_rtc_time t = {
		.year = 2026, .month = 13, .day = 1,
		.hour = 0, .minute = 0, .second = 0,
	};

	report_rc = svc_rtcdev_set(&t);
	report_done = 1;
}

ZTEST(devices, test_rtc_read_and_set_are_separate)
{
	expect_rc("r_np", rtc_read_ungranted, APP_CAP_LOG, -EPERM);
	expect_rc("r_ns", rtc_set_without_set_cap, APP_CAP_LOG | APP_CAP_RTC, -EPERM);
	expect_rc("r_bf", rtc_bad_field, APP_CAP_LOG | APP_CAP_RTC_SET, -EINVAL);
	expect_rc("r_rt", rtc_roundtrip,
		  APP_CAP_LOG | APP_CAP_RTC | APP_CAP_RTC_SET, 0);
}

/* ---- regulator ---- */

static void reg_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_regulator_count();
	report_done = 1;
}

static void reg_instance_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_regulator_enable(0, 1);
	report_done = 1;
}

static void reg_out_of_range(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_regulator_enable(99, 1);
	report_done = 1;
}

ZTEST(devices, test_regulator_gate_and_grant)
{
	expect_rc("g_np", reg_ungranted, APP_CAP_LOG, -EPERM);
	expect_rc("g_ng", reg_instance_ungranted,
		  APP_CAP_LOG | APP_CAP_REGULATOR, -EACCES);
	expect_rc("g_or", reg_out_of_range,
		  APP_CAP_LOG | APP_CAP_REGULATOR, -EINVAL);
}

/* ---- stepper ---- */

static void step_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_stepper_count();
	report_done = 1;
}

static void step_instance_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_stepper_move_by(0, 100);
	report_done = 1;
}

static void step_move_and_read(void *a)
{
	ARG_UNUSED(a);

	int32_t pos = 0;
	int rc = svc_stepper_move_by(0, 100);

	if (rc == 0) {
		rc = svc_stepper_position(0, &pos);
	}
	report_rc = rc;
	report_done = 1;
}

ZTEST(devices, test_stepper_gate_and_grant)
{
	expect_rc("s_np", step_ungranted, APP_CAP_LOG, -EPERM);
	expect_rc("s_ng", step_instance_ungranted,
		  APP_CAP_LOG | APP_CAP_STEPPER, -EACCES);

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_stepper(i, BIT(0));
	}
	expect_rc("s_ok", step_move_and_read, APP_CAP_LOG | APP_CAP_STEPPER, 0);
}

/* ---- battery state ---- */

static void power_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_battery_percent();
	report_done = 1;
}

static void power_read(void *a)
{
	ARG_UNUSED(a);

	int pct = svc_battery_percent();

	if (pct < 0) {
		report_rc = pct;
	} else {
		report_rc = (pct >= 0 && pct <= 100) ? 0 : -ERANGE;
	}
	report_done = 1;
}

ZTEST(devices, test_power_gate)
{
	expect_rc("p_np", power_ungranted, APP_CAP_LOG, -EPERM);
	expect_rc("p_ok", power_read, APP_CAP_LOG | APP_CAP_POWER, 0);
}

/* A capability above bit 31 must reach the service intact: these are the first
 * services allocated there, so this covers the wide mask end to end.
 */
ZTEST(devices, test_capability_above_bit31_reaches_service)
{
	BUILD_ASSERT(APP_CAP_POWER > UINT32_MAX, "POWER must sit above bit 31");
	BUILD_ASSERT(APP_CAP_STEPPER > UINT32_MAX, "STEPPER must sit above bit 31");

	/* Held: answers. Not held: -EPERM. Truncation would break the first. */
	expect_rc("w_ok", power_read, APP_CAP_LOG | APP_CAP_POWER, 0);
	expect_rc("w_no", power_ungranted, APP_CAP_LOG | APP_CAP_STEPPER, -EPERM);
}

/* ---- reclaim returns hardware to idle ---- */

K_APP_DMEM(report_part) volatile int holding;
K_APP_DMEM(report_part) volatile int release;

/* Launch an application that reports, then keeps running until released, so
 * the hardware it drove can be inspected before and after its reclaim.
 */
static void launch_held(const char *name, app_entry_t entry, app_caps_t caps)
{
	const struct app_def def = {
		.name = name, .source = APP_BUILTIN, .entry = entry, .caps = caps,
	};
	int id;

	report_rc = -9999;
	report_done = 0;
	holding = 0;
	release = 0;

	id = app_platform_launch(&def);
	zassert_true(id >= 0, "%s launch failed", name);
	k_mem_domain_add_partition(app_platform_domain(id), &report_part);
	for (int i = 0; i < 400 && holding == 0; i++) {
		k_msleep(5);
	}
	zassert_equal(holding, 1, "%s never reported", name);
	zassert_equal(report_rc, 0, "%s rc %d", name, report_rc);
}

static void release_held(const char *name)
{
	release = 1;
	zassert_true(wait_slot_free(name, 2000), "%s never exited", name);
}

static void hold_until_released(int rc)
{
	report_rc = rc;
	report_done = 1;
	holding = 1;
	while (release == 0) {
		k_msleep(1);
	}
}

static void reg_enable_twice(void *a)
{
	ARG_UNUSED(a);

	int rc = svc_regulator_enable(0, 1);

	if (rc == 0) {
		rc = svc_regulator_enable(0, 1);
	}
	hold_until_released(rc);
}

ZTEST(devices, test_regulator_released_on_reclaim)
{
	const struct device *reg = DEVICE_DT_GET(DT_NODELABEL(fake_reg0));

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_regulator(i, BIT(0));
	}
	launch_held("g_rc", reg_enable_twice, APP_CAP_LOG | APP_CAP_REGULATOR);
	zassert_true(regulator_is_enabled(reg), "enable did not reach the regulator");
	release_held("g_rc");
	zassert_false(regulator_is_enabled(reg), "reclaim left the regulator enabled");
}

static void reg_disable_only(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_regulator_enable(0, 0);
	report_done = 1;
}

static void reg_disable_twice(void *a)
{
	ARG_UNUSED(a);

	int rc = svc_regulator_enable(0, 1);

	if (rc == 0) {
		rc = svc_regulator_enable(0, 0);
	}
	if (rc == 0) {
		rc = svc_regulator_enable(0, 0);
	}
	report_rc = rc;
	report_done = 1;
}

/* A reference the system holds must survive an application's unmatched
 * disables.
 */
ZTEST(devices, test_regulator_disable_needs_own_enable)
{
	const struct device *reg = DEVICE_DT_GET(DT_NODELABEL(fake_reg0));

	zassert_ok(regulator_enable(reg));

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_regulator(i, BIT(0));
	}
	expect_rc("g_do", reg_disable_only, APP_CAP_LOG | APP_CAP_REGULATOR, -EALREADY);
	zassert_true(regulator_is_enabled(reg), "system reference was dropped");

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_regulator(i, BIT(0));
	}
	expect_rc("g_dt", reg_disable_twice, APP_CAP_LOG | APP_CAP_REGULATOR, -EALREADY);
	zassert_true(regulator_is_enabled(reg), "system reference was dropped");

	zassert_ok(regulator_disable(reg));
	zassert_false(regulator_is_enabled(reg));
}

static void step_move_only(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_stepper_move_by(0, 100);
	report_done = 1;
}

ZTEST(devices, test_stepper_stopped_on_reclaim)
{
	const struct device *step = DEVICE_DT_GET(DT_NODELABEL(fake_stepper));
	unsigned int stops = fake_stepper_ctrl_stop_fake.call_count;

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_stepper(i, BIT(0));
	}
	expect_rc("s_rc", step_move_only, APP_CAP_LOG | APP_CAP_STEPPER, 0);
	zassert_equal(fake_stepper_ctrl_stop_fake.call_count, stops + 1U,
		      "reclaim did not stop the stepper");
	zassert_equal_ptr(fake_stepper_ctrl_stop_fake.arg0_val, step);
}

static void dac_write_held(void *a)
{
	ARG_UNUSED(a);
	hold_until_released(svc_dac_write(0, 128));
}

ZTEST(devices, test_dac_idle_on_reclaim)
{
	const struct device *dac = DEVICE_DT_GET(DT_NODELABEL(dac_emul));
	uint32_t val = 0U;

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_dac(i, BIT(0));
	}
	launch_held("d_rc", dac_write_held, APP_CAP_LOG | APP_CAP_DAC);
	zassert_ok(dac_emul_value_get(dac, 0, &val));
	zassert_equal(val, 128U, "write did not reach the DAC");
	release_held("d_rc");
	zassert_ok(dac_emul_value_get(dac, 0, &val));
	zassert_equal(val, 0U, "reclaim left the DAC driven");
}

static void pwm_set_half(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_pwm_set(0, 500U);
	report_done = 1;
}

static void pwm_set_then_idle(void *a)
{
	ARG_UNUSED(a);

	int rc = svc_pwm_set(0, 500U);

	if (rc == 0) {
		rc = svc_pwm_set(0, 0U);
	}
	report_rc = rc;
	report_done = 1;
}

ZTEST(devices, test_pwm_idle_on_reclaim)
{
	const struct device *pwm = DEVICE_DT_GET(DT_NODELABEL(fake_pwm));

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_pwm(i, BIT(0));
	}
	expect_rc("w_rc", pwm_set_half, APP_CAP_LOG | APP_CAP_PWM, 0);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, 2U,
		      "reclaim did not reset the channel");
	zassert_equal_ptr(fake_pwm_set_cycles_fake.arg0_val, pwm);
	zassert_equal(fake_pwm_set_cycles_fake.arg1_val, 0U);
	zassert_equal(fake_pwm_set_cycles_fake.arg2_val, 1000U);
	zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 0U);

	/* A channel the application already returned to idle is left alone. */
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_pwm(i, BIT(0));
	}
	expect_rc("w_id", pwm_set_then_idle, APP_CAP_LOG | APP_CAP_PWM, 0);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, 4U);
}

static void motor_set_half(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_motor_set(1U, 50U);
	report_done = 1;
}

ZTEST(devices, test_motor_idle_on_reclaim)
{
	const struct device *mcpwm = DEVICE_DT_GET(DT_NODELABEL(mcpwm0));

	expect_rc("m_rc", motor_set_half, APP_CAP_LOG | APP_CAP_MOTOR, 0);
	zassert_equal(fake_pwm_set_cycles_fake.call_count, 2U,
		      "reclaim did not reset the channel");
	zassert_equal_ptr(fake_pwm_set_cycles_fake.arg0_val, mcpwm);
	zassert_equal(fake_pwm_set_cycles_fake.arg1_val, 1U);
	zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 0U);
}

static void gpio_set_held(void *a)
{
	ARG_UNUSED(a);
	hold_until_released(svc_gpio_set(0, 1));
}

ZTEST(devices, test_gpio_inactive_on_reclaim)
{
	const struct gpio_dt_spec line = GPIO_DT_SPEC_GET(DT_NODELABEL(test_line), gpios);

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_gpio(i, BIT(0));
	}
	launch_held("l_rc", gpio_set_held, APP_CAP_LOG | APP_CAP_GPIO);
	zassert_equal(gpio_emul_output_get_dt(&line), 1, "line was not driven");
	release_held("l_rc");
	zassert_equal(gpio_emul_output_get_dt(&line), 0, "reclaim left the line active");
}

static void led_set_held(void *a)
{
	ARG_UNUSED(a);
	hold_until_released(svc_led_set(0, 1));
}

ZTEST(devices, test_led_off_on_reclaim)
{
	const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(test_led), gpios);

	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_led(i, BIT(0));
	}
	launch_held("o_rc", led_set_held, APP_CAP_LOG | APP_CAP_LED);
	zassert_equal(gpio_emul_output_get_dt(&led), 1, "LED was not lit");
	release_held("o_rc");
	zassert_equal(gpio_emul_output_get_dt(&led), 0, "reclaim left the LED lit");
}

/* ---- the per-application file sandbox ---- */

static void fs_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_file_open("x.txt", 1);
	report_done = 1;
}

static void fs_escape_absolute(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_file_open("/lfs/apps/0/secret.txt", 1);
	report_done = 1;
}

static void fs_escape_dotdot(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_file_open("../0/secret.txt", 1);
	report_done = 1;
}

static void fs_roundtrip(void *a)
{
	ARG_UNUSED(a);

	int h = svc_file_open("note.txt", 1);

	if (h < 0) {
		report_rc = h;
		report_done = 1;
		return;
	}

	char buf[8] = {0};
	int rc = svc_file_write(h, "abcd", 4);

	if (rc == 4) {
		rc = svc_file_seek(h, 0, FS_SEEK_SET);
		if (rc == 0) {
			rc = svc_file_read(h, buf, sizeof(buf));
			rc = (rc == 4 && memcmp(buf, "abcd", 4) == 0) ? 0 :
			     (rc < 0 ? rc : -EIO);
		}
	} else if (rc >= 0) {
		rc = -EIO;
	}
	(void)svc_file_close(h);
	report_rc = rc;
	report_done = 1;
}

/* A handle is meaningful only to the application that opened it: the table is
 * shared, so a neighbour must not reach a file by guessing a small integer.
 */
K_APP_DMEM(report_part) volatile int leaked_handle;

static void fs_leaks_handle(void *a)
{
	ARG_UNUSED(a);

	int h = svc_file_open("mine.txt", 1);

	leaked_handle = h;
	report_rc = h < 0 ? h : 0;
	report_done = 1;
	/* Left open on purpose: the reclaim must close it. */
}

static void fs_steal_handle(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_file_write(leaked_handle, "x", 1);
	report_done = 1;
}

ZTEST(devices, test_fs_gate_and_confinement)
{
	expect_rc("f_np", fs_ungranted, APP_CAP_LOG, -EPERM);
	expect_rc("f_ab", fs_escape_absolute, APP_CAP_LOG | APP_CAP_FS, -EACCES);
	expect_rc("f_dd", fs_escape_dotdot, APP_CAP_LOG | APP_CAP_FS, -EACCES);
	expect_rc("f_rt", fs_roundtrip, APP_CAP_LOG | APP_CAP_FS, 0);
}

static void fs_probe_note(void *a)
{
	ARG_UNUSED(a);

	int h = svc_file_open("note.txt", 0);

	if (h >= 0) {
		(void)svc_file_close(h);
	}
	report_rc = h < 0 ? h : 0;
	report_done = 1;
}

ZTEST(devices, test_fs_root_follows_the_name)
{
	expect_rc("f_rt", fs_roundtrip, APP_CAP_LOG | APP_CAP_FS, 0);
	expect_rc("f_rt", fs_probe_note, APP_CAP_LOG | APP_CAP_FS, 0);
	expect_rc("f_ot", fs_probe_note, APP_CAP_LOG | APP_CAP_FS, -ENOENT);
}

ZTEST(devices, test_fs_handles_are_not_shared)
{
	leaked_handle = -1;
	expect_rc("f_lk", fs_leaks_handle, APP_CAP_LOG | APP_CAP_FS, 0);
	zassert_true(leaked_handle >= 0, "no handle was opened");

	/* The owner was reclaimed, so its handle must have been closed for it
	 * rather than left for the next tenant of the table.
	 */
	expect_rc("f_st", fs_steal_handle, APP_CAP_LOG | APP_CAP_FS, -EBADF);
}

/* The file service works within a filesystem the system mounts, so the suite
 * provides one rather than the service owning a mount of its own.
 */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(test_lfs);
static struct fs_mount_t test_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &test_lfs,
	.storage_dev = (void *)PARTITION_ID(storage_partition),
	.mnt_point = "/lfs",
};

static void *devices_setup(void)
{
	int rc = fs_mount(&test_mnt);

	if (rc == -ENOENT || rc == -EFAULT || rc == -EIO) {
		/* Nothing usable there yet: format by mounting after an erase. */
		rc = fs_mount(&test_mnt);
	}
	zassert_true(rc == 0 || rc == -EBUSY, "test filesystem mount failed: %d", rc);

	zassert_equal(app_platform_init(), 0, "platform init failed");
	return NULL;
}

ZTEST_SUITE(devices, NULL, devices_setup, NULL, NULL, NULL);
