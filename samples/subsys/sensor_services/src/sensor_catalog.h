/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SENSOR_SERVICES_SENSOR_CATALOG_H_
#define SENSOR_SERVICES_SENSOR_CATALOG_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

/*
 * Sensor service catalog. The platform ANNOUNCES a list of typed sensor
 * services. An app asks for a value by type (e.g. SENSOR_CHAN_AMBIENT_TEMP) and
 * never touches the underlying peripheral, bus, or driver. Read-only sensor
 * services are shared by all apps reading concurrently - like the network
 * service in M2, but simpler because reads cannot conflict.
 *
 * A catalog entry is "present" when this board actually has a device backing
 * that channel (bound from the base firmware's devicetree); otherwise it is
 * "announced" - the platform knows the type but has no hardware for it here.
 * Apps query the catalog to discover what is available before reading.
 */

struct sensor_svc {
	const char         *name;        /* human-facing service name        */
	enum sensor_channel chan;        /* the Zephyr typed channel         */
	const char         *unit;        /* unit of the returned value       */
	bool                present;     /* backed by a real device here?    */
};

/* Initialize the catalog (bus lock). Call once at startup. */
void sensor_catalog_init(void);

/* Number of announced sensor services. */
int sensor_catalog_count(void);

/* Get a catalog entry by index (read-only, copies metadata out). */
int sensor_catalog_get(int idx, struct sensor_svc *out);

/*
 * Read a sensor value by catalog index. Returns the value in milli-units
 * (e.g. milli-degrees C). Safe to call from many apps in parallel; reads are
 * serialized internally on the bus but appear concurrent to callers. Returns
 * -ENODEV if the service is announced but not present on this board.
 */
int sensor_catalog_read(int idx, int32_t *milli_value);

#endif /* SENSOR_SERVICES_SENSOR_CATALOG_H_ */
