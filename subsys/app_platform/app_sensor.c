/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Typed sensor services. The platform announces a catalog of sensor kinds; an
 * application asks for a value by index and never touches the peripheral, the
 * bus or the driver behind it. An entry is present when this board has a device
 * backing that kind, and announced otherwise, so the same application runs on a
 * board that gains the hardware later without being rebuilt.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <errno.h>

/* The board's die-temperature device. The node has to be enabled, not merely
 * present: several SoC files declare the alias while leaving the device
 * disabled, and DEVICE_DT_GET on a disabled node does not link.
 */
#if DT_NODE_HAS_STATUS(DT_ALIAS(die_temp0), okay)
#define HAVE_DIE_TEMP 1
static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));
#else
#define HAVE_DIE_TEMP 0
#endif

struct sensor_svc {
	const char         *name;
	enum sensor_channel chan;
	const char         *unit;
	bool                present;
};

static struct sensor_svc catalog[] = {
	/* Environment */
	{ "temperature",      SENSOR_CHAN_AMBIENT_TEMP, "m.degC",   HAVE_DIE_TEMP },
	{ "humidity",         SENSOR_CHAN_HUMIDITY,     "m.%RH",    false },
	{ "pressure",         SENSOR_CHAN_PRESS,        "m.kPa",    false },
	{ "altitude",         SENSOR_CHAN_ALTITUDE,     "mm",       false },
	{ "gas-resistance",   SENSOR_CHAN_GAS_RES,      "ohm",      false },
	{ "voc",              SENSOR_CHAN_VOC,          "ppb",      false },

	/* Light / optical */
	{ "ambient-light",    SENSOR_CHAN_AMBIENT_LIGHT, "m.lux",    false },
	{ "ir",               SENSOR_CHAN_IR,           "raw",      false },
	{ "red",              SENSOR_CHAN_RED,          "raw",      false },
	{ "green",            SENSOR_CHAN_GREEN,        "raw",      false },
	{ "blue",             SENSOR_CHAN_BLUE,         "raw",      false },
	{ "proximity",        SENSOR_CHAN_PROX,         "raw",      false },
	{ "distance",         SENSOR_CHAN_DISTANCE,     "mm",       false },

	/* Motion (IMU) */
	{ "accel-x",          SENSOR_CHAN_ACCEL_X,      "m.m/s2",   false },
	{ "accel-y",          SENSOR_CHAN_ACCEL_Y,      "m.m/s2",   false },
	{ "accel-z",          SENSOR_CHAN_ACCEL_Z,      "m.m/s2",   false },
	{ "gyro-x",           SENSOR_CHAN_GYRO_X,       "m.dps",    false },
	{ "gyro-y",           SENSOR_CHAN_GYRO_Y,       "m.dps",    false },
	{ "gyro-z",           SENSOR_CHAN_GYRO_Z,       "m.dps",    false },
	{ "magn-x",           SENSOR_CHAN_MAGN_X,       "m.gauss",  false },
	{ "magn-y",           SENSOR_CHAN_MAGN_Y,       "m.gauss",  false },
	{ "magn-z",           SENSOR_CHAN_MAGN_Z,       "m.gauss",  false },
	{ "rotation",         SENSOR_CHAN_ROTATION,     "m.deg",    false },

	/* Power / electrical */
	{ "voltage",          SENSOR_CHAN_VOLTAGE,      "mV",       false },
	{ "current",          SENSOR_CHAN_CURRENT,      "mA",       false },
	{ "power",            SENSOR_CHAN_POWER,        "mW",       false },
	{ "resistance",       SENSOR_CHAN_RESISTANCE,   "ohm",      false },

	/* Mechanical / rate */
	{ "rpm",              SENSOR_CHAN_RPM,          "rpm",      false },
	{ "frequency",        SENSOR_CHAN_FREQUENCY,    "mHz",      false },
	{ "flow-rate",        SENSOR_CHAN_FLOW_RATE,    "m.L/min",  false },

	/* Battery / fuel gauge */
	{ "batt-voltage",     SENSOR_CHAN_GAUGE_VOLTAGE,        "mV",   false },
	{ "batt-current",     SENSOR_CHAN_GAUGE_AVG_CURRENT,    "mA",   false },
	{ "batt-charge",      SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, "m.%",  false },
	{ "batt-health",      SENSOR_CHAN_GAUGE_STATE_OF_HEALTH, "m.%",  false },
	{ "batt-temp",        SENSOR_CHAN_GAUGE_TEMP,           "m.degC", false },
};

#define CATALOG_COUNT ((int)ARRAY_SIZE(catalog))

/* One transaction at a time on the shared device; readers still appear
 * concurrent to the applications calling in.
 */
static K_MUTEX_DEFINE(sensor_lock);

static inline bool app_has_cap(uint32_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

int z_impl_svc_sensor_count(void)
{
	if (!app_has_cap(APP_CAP_SENSOR)) {
		return -EPERM;
	}
	return CATALOG_COUNT;
}

int z_impl_svc_sensor_info(int idx, struct svc_sensor_info *out)
{
	if (!app_has_cap(APP_CAP_SENSOR)) {
		return -EPERM;
	}
	if (idx < 0 || idx >= CATALOG_COUNT) {
		return -EINVAL;
	}

	struct svc_sensor_info info;

	memset(&info, 0, sizeof(info));
	(void)strncpy(info.name, catalog[idx].name, sizeof(info.name) - 1);
	(void)strncpy(info.unit, catalog[idx].unit, sizeof(info.unit) - 1);
	info.present = catalog[idx].present;
	*out = info;
	return 0;
}

int z_impl_svc_sensor_read(int idx, int32_t *milli_value)
{
	if (!app_has_cap(APP_CAP_SENSOR)) {
		return -EPERM;
	}
	if (idx < 0 || idx >= CATALOG_COUNT) {
		return -EINVAL;
	}
	if (!catalog[idx].present) {
		return -ENODEV;
	}
#if HAVE_DIE_TEMP
	if (!device_is_ready(die_temp)) {
		return -ENODEV;
	}
	if (k_mutex_lock(&sensor_lock, K_SECONDS(2)) != 0) {
		return -EBUSY;
	}

	int rc = sensor_sample_fetch(die_temp);

	if (rc == 0) {
		struct sensor_value val;

		rc = sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &val);
		if (rc == 0) {
			*milli_value = val.val1 * 1000 + val.val2 / 1000;
		}
	}
	k_mutex_unlock(&sensor_lock);
	return rc;
#else
	return -ENODEV;
#endif
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_svc_sensor_count(void)
{
	return z_impl_svc_sensor_count();
}
#include <zephyr/syscalls/svc_sensor_count_mrsh.c>

static inline int z_vrfy_svc_sensor_info(int idx, struct svc_sensor_info *out)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));

	struct svc_sensor_info info;
	int rc = z_impl_svc_sensor_info(idx, &info);

	if (rc == 0) {
		K_OOPS(k_usermode_to_copy(out, &info, sizeof(info)));
	}
	return rc;
}
#include <zephyr/syscalls/svc_sensor_info_mrsh.c>

static inline int z_vrfy_svc_sensor_read(int idx, int32_t *milli_value)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(milli_value, sizeof(*milli_value)));

	int32_t v = 0;
	int rc = z_impl_svc_sensor_read(idx, &v);

	if (rc == 0) {
		K_OOPS(k_usermode_to_copy(milli_value, &v, sizeof(v)));
	}
	return rc;
}
#include <zephyr/syscalls/svc_sensor_read_mrsh.c>

#endif /* CONFIG_USERSPACE */
