/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SENSOR_SERVICES_SENSOR_BROKER_H_
#define SENSOR_SERVICES_SENSOR_BROKER_H_

#include <zephyr/kernel.h>

/*
 * Broker syscalls the apps use to discover and read announced sensor services.
 * The catalog and the underlying devices live in the trusted base; apps only
 * pass indices and small buffers. Reads are read-only and shareable, so any
 * number of apps may read in parallel.
 */

/* How many sensor services the platform announces. */
__syscall int svc_sensor_count(void);

/*
 * Discover service @p idx: copies the name (up to name_sz) and the unit (up to
 * unit_sz) into the caller's buffers, and writes whether it is present here.
 * Returns 0 on success.
 */
__syscall int svc_sensor_info(int idx, char *name, size_t name_sz,
			      char *unit, size_t unit_sz, bool *present);

/*
 * Read service @p idx; writes the value in milli-units. Returns 0 on success,
 * -ENODEV if announced but not present on this board.
 */
__syscall int svc_sensor_read(int idx, int32_t *milli_value);

#include <zephyr/syscalls/sensor_broker.h>

#endif /* SENSOR_SERVICES_SENSOR_BROKER_H_ */
