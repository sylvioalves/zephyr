/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_I2C_SCOPED_I2C_SVC_H_
#define APP_I2C_SCOPED_I2C_SVC_H_

#include <zephyr/kernel.h>

/*
 * Scoped I2C capability - the Tier-2 service pattern. A bus is powerful: raw
 * access lets an app talk to ANY device on it, including the system's own
 * chips. So apps never get raw I2C. Instead each app's manifest grants specific
 * (bus, address) pairs, and the broker enforces the scope BEFORE any bus
 * transaction. A call to a granted address proceeds; any other address is
 * denied without ever touching the bus. CAN, SPI, UART, and flash follow the
 * same scoped-capability shape.
 */

/* Supervisor: grant an app thread access to one (bus, addr). */
void i2c_grant(k_tid_t tid, uint8_t bus, uint16_t addr);

/* App: read one register. Returns the byte (>=0), -EPERM if not granted. */
__syscall int app_i2c_read(uint8_t bus, uint16_t addr, uint8_t reg);

#include <zephyr/syscalls/i2c_svc.h>

#endif /* APP_I2C_SCOPED_I2C_SVC_H_ */
