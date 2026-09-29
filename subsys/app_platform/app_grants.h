/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_
#define ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_

#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_platform.h>

#define APP_PLATFORM_NAME_LEN 24

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS

/* Bound on the wait for a service lock. The protected sections are short, so
 * reaching it means the resource is wedged and the service returns -EBUSY
 * instead of parking the caller.
 */
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

/* True when the calling application holds every capability in @p mask. */
static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* A lock that an application thread takes inside a service. When the thread
 * dies holding it, the service state it guards may be half updated, so the
 * lock is marked broken and refuses every later caller.
 */
struct svc_lock {
	struct k_mutex mutex;
	atomic_t broken;
};

#define SVC_LOCK_DEFINE(name) \
	struct svc_lock name __maybe_unused = { .mutex = Z_MUTEX_INITIALIZER(name.mutex) }

/* Service lock operations. A stop of an application that holds a service lock
 * takes effect once it releases the last one. Taking a broken lock fails with
 * -EIO.
 */
int app_svc_lock(struct svc_lock *lock, k_timeout_t timeout);
int app_svc_unlock(struct svc_lock *lock);

/* Per-resource grants live in the service that owns the resource, indexed by
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

#ifdef CONFIG_APP_PLATFORM_RGB
void app_platform_clear_rgb_grants(int id);
#else
static inline void app_platform_clear_rgb_grants(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_DISPLAY
void app_platform_clear_display_grants(int id);
#else
static inline void app_platform_clear_display_grants(int id)
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
void app_platform_clear_ipc(int id);
#else
static inline void app_platform_clear_flash_grants(int id)
{
	ARG_UNUSED(id);
}
static inline void app_platform_clear_ipc(int id)
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
