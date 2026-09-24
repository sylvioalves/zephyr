/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Analog output and the application's EEPROM window. Both are scoped
 * resources: a DAC channel is granted per index, and an application's EEPROM
 * access is confined to a window the platform assigns, so one application
 * cannot read or overwrite another's stored bytes.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>
#include <string.h>

#include "app_grants.h"

#define MAX_APPS CONFIG_APP_PLATFORM_MAX_APPS
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

/* ---- analog output ---- */

#ifdef CONFIG_APP_PLATFORM_DAC
#include <zephyr/drivers/dac.h>

#define DAC_ENTRY(node) DEVICE_DT_GET(node),

static const struct device *const dacs[] = {
	DT_FOREACH_STATUS_OKAY(zephyr_dac_emul, DAC_ENTRY)
};

#define DAC_COUNT ARRAY_SIZE(dacs)

static uint32_t dac_grant[MAX_APPS];
static bool dac_ready[MAX_APPS];
/* Bit N set means this application last wrote a non-zero code to channel N. */
static uint32_t dac_driven[MAX_APPS];
static K_MUTEX_DEFINE(dac_lock);

void app_platform_grant_dac(int id, uint32_t ch_mask)
{
	if (id >= 0 && id < MAX_APPS) {
		dac_grant[id] = ch_mask;
	}
}

void app_platform_clear_dac_grants(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	dac_grant[id] = 0U;
	dac_ready[id] = false;
	if (app_svc_lock(&dac_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
	for (int i = 0; i < (int)DAC_COUNT; i++) {
		if ((dac_driven[id] & BIT(i)) != 0U) {
			(void)dac_write_value(dacs[i], 0, 0U);
		}
	}
	dac_driven[id] = 0U;
	(void)app_svc_unlock(&dac_lock);
}

int z_impl_svc_dac_count(void)
{
	if (!app_has_cap(APP_CAP_DAC)) {
		return -EPERM;
	}
	return (int)DAC_COUNT;
}

/* The channel is set up on first use rather than at init, because the platform
 * does not know which application will be granted which channel.
 */
int z_impl_svc_dac_write(int ch, uint32_t value)
{
	if (!app_has_cap(APP_CAP_DAC)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || ch < 0 || ch >= (int)DAC_COUNT) {
		return -EINVAL;
	}
	if ((dac_grant[id] & BIT(ch)) == 0U) {
		return -EACCES;
	}
	if (!device_is_ready(dacs[ch])) {
		return -ENODEV;
	}

	if (!dac_ready[id]) {
		const struct dac_channel_cfg cfg = {
			.channel_id = 0,
			.resolution = CONFIG_APP_PLATFORM_DAC_RESOLUTION,
			.buffered = false,
		};
		int rc = dac_channel_setup(dacs[ch], &cfg);

		if (rc != 0) {
			return rc;
		}
		dac_ready[id] = true;
	}
	if (app_svc_lock(&dac_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}

	int rc = dac_write_value(dacs[ch], 0, value);

	if (rc == 0) {
		for (int i = 0; i < MAX_APPS; i++) {
			dac_driven[i] &= ~BIT(ch);
		}
		if (value != 0U) {
			dac_driven[id] |= BIT(ch);
		}
	}
	(void)app_svc_unlock(&dac_lock);
	return rc;
}
#endif /* CONFIG_APP_PLATFORM_DAC */

/* ---- EEPROM window ---- */

#ifdef CONFIG_APP_PLATFORM_EEPROM
#include <zephyr/drivers/eeprom.h>

static const struct device *const eeprom_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_eeprom));

struct eeprom_window {
	size_t base;
	size_t len;
};

static struct eeprom_window eeprom_grant[MAX_APPS];

void app_platform_grant_eeprom(int id, size_t base, size_t len)
{
	if (id >= 0 && id < MAX_APPS) {
		eeprom_grant[id].base = base;
		eeprom_grant[id].len = len;
	}
}

void app_platform_clear_eeprom_grants(int id)
{
	if (id >= 0 && id < MAX_APPS) {
		eeprom_grant[id].base = 0;
		eeprom_grant[id].len = 0;
	}
}

/* Translate an application-relative offset into the device, refusing anything
 * that leaves the window. The length is checked against the remaining window
 * rather than the sum, so an offset near SIZE_MAX cannot wrap past the end.
 */
static int eeprom_window(size_t off, size_t len, size_t *abs)
{
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EPERM;
	}

	const struct eeprom_window *w = &eeprom_grant[id];

	if (w->len == 0) {
		return -EACCES;
	}
	if (off > w->len || len > w->len - off) {
		return -EINVAL;
	}
	*abs = w->base + off;
	return 0;
}

int z_impl_svc_eeprom_size(void)
{
	if (!app_has_cap(APP_CAP_EEPROM)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EPERM;
	}
	return (int)eeprom_grant[id].len;
}

int z_impl_svc_eeprom_read(uint32_t off, void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_EEPROM)) {
		return -EPERM;
	}
	if (len > CONFIG_APP_PLATFORM_EEPROM_XFER_MAX) {
		return -EINVAL;
	}

	size_t abs = 0;
	int rc = eeprom_window(off, len, &abs);

	if (rc != 0) {
		return rc;
	}
	if (!device_is_ready(eeprom_dev)) {
		return -ENODEV;
	}
	rc = eeprom_read(eeprom_dev, (off_t)abs, buf, len);
	return rc != 0 ? rc : (int)len;
}

int z_impl_svc_eeprom_write(uint32_t off, const void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_EEPROM)) {
		return -EPERM;
	}
	if (len > CONFIG_APP_PLATFORM_EEPROM_XFER_MAX) {
		return -EINVAL;
	}

	size_t abs = 0;
	int rc = eeprom_window(off, len, &abs);

	if (rc != 0) {
		return rc;
	}
	if (!device_is_ready(eeprom_dev)) {
		return -ENODEV;
	}
	rc = eeprom_write(eeprom_dev, (off_t)abs, buf, len);
	return rc != 0 ? rc : (int)len;
}
#endif /* CONFIG_APP_PLATFORM_EEPROM */

#ifdef CONFIG_USERSPACE
#ifdef CONFIG_APP_PLATFORM_DAC
static inline int z_vrfy_svc_dac_count(void)
{
	return z_impl_svc_dac_count();
}
#include <zephyr/syscalls/svc_dac_count_mrsh.c>

static inline int z_vrfy_svc_dac_write(int ch, uint32_t value)
{
	return z_impl_svc_dac_write(ch, value);
}
#include <zephyr/syscalls/svc_dac_write_mrsh.c>
#endif /* CONFIG_APP_PLATFORM_DAC */

#ifdef CONFIG_APP_PLATFORM_EEPROM
static inline int z_vrfy_svc_eeprom_size(void)
{
	return z_impl_svc_eeprom_size();
}
#include <zephyr/syscalls/svc_eeprom_size_mrsh.c>

static inline int z_vrfy_svc_eeprom_read(uint32_t off, void *buf, size_t len)
{
	char tmp[CONFIG_APP_PLATFORM_EEPROM_XFER_MAX];

	if (len > sizeof(tmp)) {
		return -EINVAL;
	}

	int rc = z_impl_svc_eeprom_read(off, tmp, len);

	if (rc > 0 && k_usermode_to_copy(buf, tmp, rc) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/svc_eeprom_read_mrsh.c>

static inline int z_vrfy_svc_eeprom_write(uint32_t off, const void *buf, size_t len)
{
	char tmp[CONFIG_APP_PLATFORM_EEPROM_XFER_MAX];

	if (len > sizeof(tmp)) {
		return -EINVAL;
	}
	if (k_usermode_from_copy(tmp, (void *)buf, len) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_eeprom_write(off, tmp, len);
}
#include <zephyr/syscalls/svc_eeprom_write_mrsh.c>
#endif /* CONFIG_APP_PLATFORM_EEPROM */
#endif /* CONFIG_USERSPACE */
