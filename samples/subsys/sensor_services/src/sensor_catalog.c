/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sensor_catalog.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

/*
 * The board's real die-temperature sensor, bound from devicetree. This is the
 * one entry actually backed by hardware on this board; every read of the
 * AMBIENT/DIE temperature service goes through it.
 */
#define HAVE_DIE_TEMP DT_NODE_EXISTS(DT_ALIAS(die_temp0))
#if HAVE_DIE_TEMP
static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));
#endif

/* One mutex serializes the actual bus transaction; reads still appear parallel. */
static struct k_mutex bus_lock;

/*
 * The announced catalog. Most entries are "announced, not present here": the
 * platform knows the type, but this board has no device for it. Attach the
 * matching peripheral (and declare it in the base firmware's devicetree), and
 * the same service becomes live without any app change. The temperature entry
 * is present on this board.
 */
static struct sensor_svc catalog[] = {
	/* Environment */
	{ "temperature",      SENSOR_CHAN_AMBIENT_TEMP, "m.degC",   true  },
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

void sensor_catalog_init(void)
{
	k_mutex_init(&bus_lock);
}

int sensor_catalog_count(void)
{
	return CATALOG_COUNT;
}

int sensor_catalog_get(int idx, struct sensor_svc *out)
{
	if (idx < 0 || idx >= CATALOG_COUNT) {
		return -EINVAL;
	}
	*out = catalog[idx];
	return 0;
}

int sensor_catalog_read(int idx, int32_t *milli_value)
{
	if (idx < 0 || idx >= CATALOG_COUNT) {
		return -EINVAL;
	}
	if (!catalog[idx].present) {
		return -ENODEV;
	}

#if HAVE_DIE_TEMP
	int rc;

	/* Serialize the real bus transaction; callers still appear parallel. */
	k_mutex_lock(&bus_lock, K_FOREVER);

	rc = sensor_sample_fetch(die_temp);
	if (rc == 0) {
		struct sensor_value val;

		rc = sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &val);
		if (rc == 0) {
			*milli_value = val.val1 * 1000 + val.val2 / 1000;
		}
	}

	k_mutex_unlock(&bus_lock);
	return rc;
#else
	return -ENODEV;
#endif
}
