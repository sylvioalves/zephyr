/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The platform: one service registry and the SDK syscalls behind it. Every
 * service the platform exposes is one registry row with a name and a capability
 * tier; the SDK functions enforce the tier and forward to the real driver or
 * subsystem. This is the single place the platform's surface and policy live.
 */

#include "app_sdk.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/internal/syscall_handler.h>
#include <string.h>
#include <errno.h>

enum tier { TIER_OPEN = 0, TIER_SCOPED = 1, TIER_ADMIN = 2 };

/* The service registry: the platform's announced surface + its policy. */
struct service {
	const char *name;
	enum tier   tier;
};

static const struct service registry[] = {
	{ "log",          TIER_OPEN },
	{ "sleep",        TIER_OPEN },
	{ "thread-spawn", TIER_OPEN },
	{ "sensor",       TIER_OPEN },
	{ "i2c",          TIER_SCOPED },
	{ "wifi-config",  TIER_ADMIN },
};
#define SERVICE_COUNT ((int)ARRAY_SIZE(registry))

/* Per-app capability state, keyed by thread. */
struct app_caps {
	k_tid_t  tid;
	bool     is_admin;
	uint8_t  i2c_bus;       /* the scoped i2c grant: bus + addr */
	uint16_t i2c_addr;
	bool     i2c_granted;
};

#define MAX_APPS 4
static struct app_caps caps[MAX_APPS];

void platform_app_register(k_tid_t tid, bool admin)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (caps[i].tid == NULL) {
			caps[i].tid = tid;
			caps[i].is_admin = admin;
			return;
		}
	}
}

void platform_grant_i2c(k_tid_t tid, uint8_t bus, uint16_t addr)
{
	for (int i = 0; i < MAX_APPS; i++) {
		if (caps[i].tid == tid) {
			caps[i].i2c_bus = bus;
			caps[i].i2c_addr = addr;
			caps[i].i2c_granted = true;
			return;
		}
	}
}

static struct app_caps *caps_of_current(void)
{
	k_tid_t cur = k_current_get();

	for (int i = 0; i < MAX_APPS; i++) {
		if (caps[i].tid == cur) {
			return &caps[i];
		}
	}
	return NULL;
}

/* ---- runtime ---- */

void z_impl_app_log(const char *msg)
{
	printk("    [app] %s\n", msg);
}

void z_impl_app_sleep_ms(uint32_t ms)
{
	k_msleep((int32_t)ms);
}

uint32_t z_impl_app_uptime_ms(void)
{
	return (uint32_t)k_uptime_get();
}

/* thread spawn lives in app_rt.c; declared here for the registry view. */
extern int app_rt_spawn(app_entry_t entry, void *arg);

int z_impl_app_thread_spawn(app_entry_t entry, void *arg)
{
	return app_rt_spawn(entry, arg);
}

/* ---- discovery ---- */

int z_impl_app_service_count(void)
{
	return SERVICE_COUNT;
}

int z_impl_app_service_info(int idx, char *name, size_t name_sz, int *tier)
{
	if (idx < 0 || idx >= SERVICE_COUNT) {
		return -EINVAL;
	}
	strncpy(name, registry[idx].name, name_sz - 1);
	name[name_sz - 1] = '\0';
	*tier = (int)registry[idx].tier;
	return 0;
}

/* ---- open: sensor ---- */

static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));

int z_impl_app_sensor_read(const char *name, int32_t *milli_value)
{
	if (strcmp(name, "temperature") != 0 || !device_is_ready(die_temp)) {
		return -ENODEV;
	}
	if (sensor_sample_fetch(die_temp) != 0) {
		return -EIO;
	}
	struct sensor_value v;

	if (sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v) != 0) {
		return -EIO;
	}
	*milli_value = v.val1 * 1000 + v.val2 / 1000;
	return 0;
}

/* ---- scoped: i2c ---- */

int z_impl_app_i2c_read(uint8_t bus, uint16_t addr, uint8_t reg)
{
	ARG_UNUSED(reg);
	struct app_caps *c = caps_of_current();

	if (!c || !c->i2c_granted || c->i2c_bus != bus || c->i2c_addr != addr) {
		printk("    [platform] DENIED i2c%u @ 0x%02x (not in app manifest)\n",
		       bus, addr);
		return -EPERM;
	}
	printk("    [platform] GRANTED i2c%u @ 0x%02x (scoped capability)\n",
	       bus, addr);
	/* real read would run here; no device wired, return ok-with-zero. */
	return 0;
}

/* ---- admin: wifi config ---- */

int z_impl_app_admin_wifi_set(const char *ssid, const char *psk)
{
	struct app_caps *c = caps_of_current();

	if (!c || !c->is_admin) {
		printk("    [platform] DENIED wifi-config (app is not admin)\n");
		return -EPERM;
	}
	printk("    [platform] wifi-config applied: ssid='%s' (psk %zu)\n",
	       ssid, strlen(psk));
	return 0;
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_app_service_info(int idx, char *name, size_t name_sz,
					  int *tier)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(name, name_sz));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(tier, sizeof(*tier)));
	return z_impl_app_service_info(idx, name, name_sz, tier);
}
#include <zephyr/syscalls/app_service_info_mrsh.c>

static inline int z_vrfy_app_sensor_read(const char *name, int32_t *milli_value)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(milli_value, sizeof(*milli_value)));
	return z_impl_app_sensor_read(name, milli_value);
}
#include <zephyr/syscalls/app_sensor_read_mrsh.c>

static inline int z_vrfy_app_i2c_read(uint8_t bus, uint16_t addr, uint8_t reg)
{
	return z_impl_app_i2c_read(bus, addr, reg);
}
#include <zephyr/syscalls/app_i2c_read_mrsh.c>

static inline int z_vrfy_app_admin_wifi_set(const char *ssid, const char *psk)
{
	return z_impl_app_admin_wifi_set(ssid, psk);
}
#include <zephyr/syscalls/app_admin_wifi_set_mrsh.c>

static inline void z_vrfy_app_log(const char *msg)
{
	z_impl_app_log(msg);
}
#include <zephyr/syscalls/app_log_mrsh.c>

static inline void z_vrfy_app_sleep_ms(uint32_t ms)
{
	z_impl_app_sleep_ms(ms);
}
#include <zephyr/syscalls/app_sleep_ms_mrsh.c>

static inline uint32_t z_vrfy_app_uptime_ms(void)
{
	return z_impl_app_uptime_ms();
}
#include <zephyr/syscalls/app_uptime_ms_mrsh.c>

static inline int z_vrfy_app_thread_spawn(app_entry_t entry, void *arg)
{
	return z_impl_app_thread_spawn(entry, arg);
}
#include <zephyr/syscalls/app_thread_spawn_mrsh.c>

static inline int z_vrfy_app_service_count(void)
{
	return z_impl_app_service_count();
}
#include <zephyr/syscalls/app_service_count_mrsh.c>
#endif /* CONFIG_USERSPACE */
