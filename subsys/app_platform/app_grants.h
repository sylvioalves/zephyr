/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_
#define ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_

#include <zephyr/kernel.h>

/*
 * Per-resource grants live in the service that owns the resource, indexed by
 * application slot. A slot is reused once its previous tenant is reclaimed, so
 * the manager drops the grants before it hands the slot to a new application:
 * otherwise the newcomer would inherit devices the platform never assigned it.
 * These are internal to the subsystem and are not part of the public API.
 *
 * A service that is configured out contributes no grants, and its entry here
 * becomes an empty inline so the manager still compiles both branches of the
 * IS_ENABLED() test that calls it.
 */

#ifdef CONFIG_APP_PLATFORM_GPIO
void app_platform_clear_gpio_grants(int id);
#else
static inline void app_platform_clear_gpio_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_IO
void app_platform_clear_led_grants(int id);
#else
static inline void app_platform_clear_led_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_PERIPH
void app_platform_clear_periph_grants(int id);
#else
static inline void app_platform_clear_periph_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_BUS
void app_platform_clear_bus_grants(int id);
#else
static inline void app_platform_clear_bus_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_SERVICES
void app_platform_clear_flash_grants(int id);
#else
static inline void app_platform_clear_flash_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_COUNTER
void app_platform_clear_counter_grants(int id);
#else
static inline void app_platform_clear_counter_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_DAC
void app_platform_clear_dac_grants(int id);
#else
static inline void app_platform_clear_dac_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_EEPROM
void app_platform_clear_eeprom_grants(int id);
#else
static inline void app_platform_clear_eeprom_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_REGULATOR
void app_platform_clear_regulator_grants(int id);
#else
static inline void app_platform_clear_regulator_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_STEPPER
void app_platform_clear_stepper_grants(int id);
#else
static inline void app_platform_clear_stepper_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_FS
void app_platform_clear_fs_handles(int id);
#else
static inline void app_platform_clear_fs_handles(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_EVENTS
void app_events_clear(int id);
#else
static inline void app_events_clear(int id)
{
	ARG_UNUSED(id);
}
#endif

#endif /* ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_ */
