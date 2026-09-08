/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_CAPS_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_CAPS_H_

#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/**
 * @brief Application platform capabilities
 * @defgroup app_platform_caps Application platform capabilities
 * @ingroup app_platform
 * @{
 *
 * The canonical capability set for the shared service library. An application is
 * launched with a grant mask (see @ref app_def.caps); each shared service checks
 * its own capability bit at the syscall boundary and refuses an ungranted caller
 * with -EPERM before any work is done. The bit allocation is fixed here once so
 * that adding services never renumbers an existing grant.
 */

/** A capability grant mask, one bit per service group. */
typedef uint64_t app_caps_t;

/** Build a capability bit that is well defined across the whole mask width. */
#define APP_CAP_BIT(n) ((app_caps_t)1U << (n))

/** Capability bits, one per service group. */
#define APP_CAP_LOG     APP_CAP_BIT(0)   /**< logging and heartbeat (always useful) */
#define APP_CAP_CRYPTO  APP_CAP_BIT(1)   /**< hashing and related primitives */
#define APP_CAP_RANDOM  APP_CAP_BIT(2)   /**< random bytes */
#define APP_CAP_TIME    APP_CAP_BIT(3)   /**< uptime, cycle count, wall clock */
#define APP_CAP_SENSOR  APP_CAP_BIT(4)   /**< sensor channels */
#define APP_CAP_KV      APP_CAP_BIT(5)   /**< persistent key/value */
#define APP_CAP_FLASH   APP_CAP_BIT(6)   /**< raw storage partition */
#define APP_CAP_GPIO    APP_CAP_BIT(7)   /**< digital input/output */
#define APP_CAP_PWM     APP_CAP_BIT(8)   /**< pulse-width output */
#define APP_CAP_ADC     APP_CAP_BIT(9)   /**< analog input */
#define APP_CAP_I2C     APP_CAP_BIT(10)  /**< I2C bus access */
#define APP_CAP_SPI     APP_CAP_BIT(11)  /**< SPI bus access */
#define APP_CAP_UART    APP_CAP_BIT(12)  /**< secondary serial */
#define APP_CAP_NET     APP_CAP_BIT(13)  /**< network and MQTT */
#define APP_CAP_BLE     APP_CAP_BIT(14)  /**< Bluetooth Low Energy */
#define APP_CAP_RADIO   APP_CAP_BIT(15)  /**< 802.15.4 messaging */
#define APP_CAP_IPC     APP_CAP_BIT(16)  /**< inter-application messaging */
#define APP_CAP_INSPECT APP_CAP_BIT(17)  /**< observe other application groups */
#define APP_CAP_CONTROL APP_CAP_BIT(18)  /**< start and stop other applications */
#define APP_CAP_CAN     APP_CAP_BIT(19)  /**< CAN bus messaging */
#define APP_CAP_DMA     APP_CAP_BIT(20)  /**< memory-to-memory DMA copy */
#define APP_CAP_PSRAM   APP_CAP_BIT(21)  /**< map external RAM into the app domain */
#define APP_CAP_SOCKET  APP_CAP_BIT(22)  /**< raw UDP/TCP datagrams and name resolution */
#define APP_CAP_LED     APP_CAP_BIT(23)  /**< drive granted status-LED pins */
#define APP_CAP_INPUT   APP_CAP_BIT(24)  /**< receive button/input events */
#define APP_CAP_COUNTER APP_CAP_BIT(25)  /**< hardware pulse counter */
#define APP_CAP_MOTOR   APP_CAP_BIT(26)  /**< motor-control PWM (MCPWM) channels */
#define APP_CAP_COUNTER_DEV APP_CAP_BIT(27) /**< granted hardware counter instances */
#define APP_CAP_DAC     APP_CAP_BIT(28)  /**< analog output on granted channels */
#define APP_CAP_EEPROM  APP_CAP_BIT(29)  /**< the application's EEPROM window */
#define APP_CAP_RTC     APP_CAP_BIT(30)  /**< read the real-time clock */
#define APP_CAP_RTC_SET APP_CAP_BIT(31)  /**< set the real-time clock (device-wide) */
#define APP_CAP_W1      APP_CAP_BIT(32)  /**< 1-Wire bus transactions */
#define APP_CAP_POWER   APP_CAP_BIT(33)  /**< read charger and fuel-gauge state */
#define APP_CAP_REGULATOR APP_CAP_BIT(34) /**< granted regulator instances */
#define APP_CAP_STEPPER APP_CAP_BIT(35)  /**< granted stepper motor instances */
#define APP_CAP_FS      APP_CAP_BIT(36)  /**< files under the app's private root */

/** Highest capability bit currently allocated. */
#define APP_CAP_LAST    APP_CAP_FS

/**
 * @brief Grant every defined capability (for a fully trusted application).
 *
 * Only allocated bits are set, so an application granted everything today does
 * not silently acquire a capability added tomorrow.
 */
#define APP_CAP_ALL     ((app_caps_t)((APP_CAP_LAST << 1) - 1U))

/* The mask must stay wide enough that a bit allocated above 31 survives, and
 * APP_CAP_ALL must cover the allocated bits without granting unallocated ones.
 */
BUILD_ASSERT(sizeof(app_caps_t) * 8U >= 64U, "capability mask must be 64 bits wide");
BUILD_ASSERT(APP_CAP_BIT(32) != 0U, "a capability above bit 31 is truncated");
BUILD_ASSERT((APP_CAP_ALL & APP_CAP_LAST) != 0U, "APP_CAP_ALL misses the last capability");
BUILD_ASSERT((APP_CAP_ALL & (APP_CAP_LAST << 1)) == 0U,
	     "APP_CAP_ALL grants an unallocated capability");

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_CAPS_H_ */
