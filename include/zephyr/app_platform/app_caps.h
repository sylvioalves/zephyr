/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_CAPS_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_CAPS_H_

#include <zephyr/sys/util.h>

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

/** Capability bits, one per service group. */
#define APP_CAP_LOG     BIT(0)   /**< logging and heartbeat (always useful) */
#define APP_CAP_CRYPTO  BIT(1)   /**< hashing and related primitives */
#define APP_CAP_RANDOM  BIT(2)   /**< random bytes */
#define APP_CAP_TIME    BIT(3)   /**< uptime, cycle count, wall clock */
#define APP_CAP_SENSOR  BIT(4)   /**< sensor channels */
#define APP_CAP_KV      BIT(5)   /**< persistent key/value */
#define APP_CAP_FLASH   BIT(6)   /**< raw storage partition */
#define APP_CAP_GPIO    BIT(7)   /**< digital input/output */
#define APP_CAP_PWM     BIT(8)   /**< pulse-width output */
#define APP_CAP_ADC     BIT(9)   /**< analog input */
#define APP_CAP_I2C     BIT(10)  /**< I2C bus access */
#define APP_CAP_SPI     BIT(11)  /**< SPI bus access */
#define APP_CAP_UART    BIT(12)  /**< secondary serial */
#define APP_CAP_NET     BIT(13)  /**< network and MQTT */
#define APP_CAP_BLE     BIT(14)  /**< Bluetooth Low Energy */
#define APP_CAP_RADIO   BIT(15)  /**< 802.15.4 messaging */
#define APP_CAP_IPC     BIT(16)  /**< inter-application messaging */
#define APP_CAP_INSPECT BIT(17)  /**< observe other application groups */
#define APP_CAP_CONTROL BIT(18)  /**< start and stop other applications */
#define APP_CAP_CAN     BIT(19)  /**< CAN bus messaging */
#define APP_CAP_DMA     BIT(20)  /**< memory-to-memory DMA copy */

/** Grant every capability (for a fully trusted application). */
#define APP_CAP_ALL     (~0U)

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_CAPS_H_ */
