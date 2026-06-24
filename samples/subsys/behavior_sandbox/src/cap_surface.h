/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BEHAVIOR_SANDBOX_CAP_SURFACE_H_
#define BEHAVIOR_SANDBOX_CAP_SURFACE_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Capability surface: the only gate through which the sandboxed user-mode
 * behavior thread is allowed to touch hardware. Each capability is a real
 * Zephyr syscall, so a user thread that tries to reach hardware any other
 * way faults under PMP and is contained.
 */

/**
 * @brief Drive the controllable output pin.
 *
 * @param on true to assert the pin, false to deassert.
 * @return 0 on success, negative errno on failure.
 */
__syscall int cap_led_set(bool on);

/**
 * @brief Read the on-chip die temperature.
 *
 * @param milli_c Output, temperature in milli-degrees Celsius.
 * @return 0 on success, negative errno on failure.
 */
__syscall int cap_sensor_read(int32_t *milli_c);

/**
 * @brief Cooperative sleep available to the sandbox.
 *
 * @param ms Milliseconds to sleep.
 */
__syscall void cap_sleep(uint32_t ms);

#include <zephyr/syscalls/cap_surface.h>

#endif /* BEHAVIOR_SANDBOX_CAP_SURFACE_H_ */
