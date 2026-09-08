/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The index-keyed device services, driven against emulated devices. What is
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
