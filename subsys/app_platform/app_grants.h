/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_
#define ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_platform.h>

#define APP_PLATFORM_NAME_LEN 24

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS

/* The devicetree node that lists the devices applications may use. */
#define APP_PLATFORM_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_app_platform)

#define APP_PLATFORM_DEVICE(node, prop, idx) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node, prop, idx)),

/* The devices in property @p prop of the platform node, in index order. */
#define APP_PLATFORM_DEVICES(prop)                                                         \
	COND_CODE_1(DT_NODE_HAS_PROP(APP_PLATFORM_NODE, prop),                             \
		    (DT_FOREACH_PROP_ELEM(APP_PLATFORM_NODE, prop, APP_PLATFORM_DEVICE)), ())

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
int app_service_lock(struct svc_lock *lock, k_timeout_t timeout);
int app_service_unlock(struct svc_lock *lock);

/* Returned by app_service_lock_reclaim() for a broken lock: the caller goes on
 * without the mutex and does not unlock it.
 */
#define SVC_LOCK_FORCED 1

/* Take a service lock in a reclaim hook. On the reaper a broken lock yields
 * SVC_LOCK_FORCED instead of -EIO: its owner is dead and no thread can take it
 * again, so the hook releases the reclaimed slot's own records and mappings
 * without it. It frees no memory on that path, since the dead owner may have
 * left an allocator or the slot's record half updated. Elsewhere it behaves
 * like app_service_lock().
 */
int app_service_lock_reclaim(struct svc_lock *lock, k_timeout_t timeout);

/* Start a group for an application holding APP_CAP_CONTROL: as
 * app_platform_start(), but -EAGAIN while the group waits for the firmware to
 * start it, after a stopped launch or a failed configure hook.
 */
int app_platform_start_control(int id);

/* Points inside services where app_platform_test_point() is called. */
#define APP_TEST_POINT_PSRAM_MAP 0

#ifdef CONFIG_APP_PLATFORM_TEST_FAULT_POINTS
/* Called with the service lock held; a test overrides the empty default. */
void app_platform_test_point(int point);
#else
static inline void app_platform_test_point(int point)
{
	ARG_UNUSED(point);
}
#endif

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

#ifdef CONFIG_APP_PLATFORM_RADIO
void app_platform_clear_radio(int id);
#else
static inline void app_platform_clear_radio(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_CAN
void app_platform_clear_can(int id);
#else
static inline void app_platform_clear_can(int id)
{
	ARG_UNUSED(id);
}
#endif

#ifdef CONFIG_APP_PLATFORM_BUS
/* Take one received byte of the secondary UART, 0 on success. */
int app_platform_uart_getc(unsigned char *c);
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

void app_platform_clear_sems(int id);

/* Per-application heap regions. A carve maps the region into @p dom, which is
 * NULL without user mode; a release unmaps, zeroes and frees it; a reset zeroes
 * and re-initializes the heap of a stopped application.
 */
int app_platform_heap_carve(int id, struct k_mem_domain *dom, size_t heap_size);
void app_platform_heap_reset(int id);
void app_platform_heap_release(int id, struct k_mem_domain *dom);
size_t app_platform_heap_bytes(int id);

#ifdef CONFIG_APP_PLATFORM_EVENTS
void app_events_clear(int id);
#else
static inline void app_events_clear(int id)
{
	ARG_UNUSED(id);
}
#endif

#endif /* ZEPHYR_SUBSYS_APP_PLATFORM_APP_GRANTS_H_ */
