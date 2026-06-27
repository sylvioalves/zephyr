/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The shared BLE service. The Bluetooth controller and host live in the
 * platform; an application that holds APP_CAP_BLE asks the platform to scan for
 * a peer by advertised name and gets back a yes or no. The application never
 * touches the BLE stack, so it links none of it. Scanning is serialized so that
 * concurrent callers from several applications do not collide.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <string.h>
#include <errno.h>

#define BLE_NAME_MAX 24

static inline bool app_has_cap(uint32_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

static K_MUTEX_DEFINE(ble_lock);
static K_SEM_DEFINE(found_sem, 0, 1);
static char want_name[BLE_NAME_MAX];
static bool bt_started;

struct name_match {
	const char *want;
	bool        matched;
};

static bool parse_name_cb(struct bt_data *data, void *user)
{
	struct name_match *m = user;

	if (data->type == BT_DATA_NAME_COMPLETE || data->type == BT_DATA_NAME_SHORTENED) {
		size_t len = MIN(data->data_len, (size_t)BLE_NAME_MAX - 1);

		if (strlen(m->want) == len &&
		    strncmp((const char *)data->data, m->want, len) == 0) {
			m->matched = true;
			return false;
		}
	}
	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		    struct net_buf_simple *ad)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(rssi);
	ARG_UNUSED(type);

	struct name_match m = { .want = want_name, .matched = false };

	bt_data_parse(ad, parse_name_cb, &m);
	if (m.matched) {
		k_sem_give(&found_sem);
	}
}

int z_impl_svc_ble_scan(const char *name, uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}

	k_mutex_lock(&ble_lock, K_FOREVER);

	int rc = 0;

	if (!bt_started) {
		rc = bt_enable(NULL);
		if (rc != 0 && rc != -EALREADY) {
			goto out;
		}
		bt_started = true;
	}

	strncpy(want_name, name, sizeof(want_name) - 1);
	want_name[sizeof(want_name) - 1] = '\0';
	k_sem_reset(&found_sem);

	struct bt_le_scan_param sp = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	rc = bt_le_scan_start(&sp, scan_cb);
	if (rc != 0) {
		goto out;
	}

	rc = (k_sem_take(&found_sem, K_MSEC(timeout_ms)) == 0) ? 1 : 0;
	bt_le_scan_stop();
out:
	k_mutex_unlock(&ble_lock);
	return rc;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_ble_scan(const char *name, uint32_t timeout_ms)
{
	char buf[BLE_NAME_MAX];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_ble_scan(buf, timeout_ms);
}
#include <zephyr/syscalls/svc_ble_scan_mrsh.c>
#endif
