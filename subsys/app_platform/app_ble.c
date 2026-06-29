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
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
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

/*
 * The BLE controller and host are brought up, and every scan and GATT client
 * flow is driven, on this dedicated supervisor thread. A service syscall runs
 * on the calling user-mode application thread, where bt_enable() and the HCI
 * command path cannot complete; it only posts a request here and waits for the
 * result. The ble_lock mutex serializes callers, so a single in-flight request
 * with module-level result storage is enough.
 */
enum ble_op {
	BLE_OP_SCAN,
	BLE_OP_HR_READ,
};

struct ble_req {
	enum ble_op op;
	char        name[BLE_NAME_MAX];
	uint32_t    timeout_ms;
};

K_MSGQ_DEFINE(ble_req_q, sizeof(struct ble_req), 1, 4);
static K_SEM_DEFINE(ble_done, 0, 1);
static int ble_result;
static uint8_t ble_bpm;

static int ble_ensure_enabled(void)
{
	int rc;

	if (bt_started) {
		return 0;
	}
	rc = bt_enable(NULL);
	if (rc != 0 && rc != -EALREADY) {
		return rc;
	}
	bt_started = true;
	return 0;
}

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

/* Runs on the BLE worker thread. */
static int ble_do_scan(const char *name, uint32_t timeout_ms)
{
	int rc = ble_ensure_enabled();

	if (rc != 0) {
		return rc;
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
		return rc;
	}

	rc = (k_sem_take(&found_sem, K_MSEC(timeout_ms)) == 0) ? 1 : 0;
	bt_le_scan_stop();
	return rc;
}

int z_impl_svc_ble_scan(const char *name, uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}

	struct ble_req req = { .op = BLE_OP_SCAN, .timeout_ms = timeout_ms };

	strncpy(req.name, name, sizeof(req.name) - 1);
	req.name[sizeof(req.name) - 1] = '\0';

	k_mutex_lock(&ble_lock, K_FOREVER);
	(void)k_msgq_put(&ble_req_q, &req, K_FOREVER);
	(void)k_sem_take(&ble_done, K_FOREVER);
	int rc = ble_result;

	k_mutex_unlock(&ble_lock);
	return rc;
}

/*
 * Connect to a heart-rate peer, discover the measurement characteristic,
 * subscribe, and capture one reading. The platform owns the whole client; the
 * application gets back a single value.
 */
static struct bt_conn *hr_conn;
static struct bt_uuid_16 hr_disc_uuid = BT_UUID_INIT_16(0);
static struct bt_gatt_discover_params hr_disc;
static struct bt_gatt_subscribe_params hr_sub;
static K_SEM_DEFINE(hr_done, 0, 1);
static uint8_t hr_value;
static bool hr_connect_wanted;

static uint8_t hr_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *p,
			 const void *data, uint16_t len)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(p);
	if (data && len >= 2) {
		/* HR measurement: flags byte then the 8-bit value. */
		hr_value = ((const uint8_t *)data)[1];
		k_sem_give(&hr_done);
	}
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t hr_discover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	if (!attr) {
		return BT_GATT_ITER_STOP;
	}

	if (!bt_uuid_cmp(params->uuid, BT_UUID_HRS)) {
		memcpy(&hr_disc_uuid, BT_UUID_HRS_MEASUREMENT, sizeof(hr_disc_uuid));
		hr_disc.uuid = &hr_disc_uuid.uuid;
		hr_disc.start_handle = attr->handle + 1;
		hr_disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		bt_gatt_discover(conn, &hr_disc);
	} else if (!bt_uuid_cmp(params->uuid, BT_UUID_HRS_MEASUREMENT)) {
		memcpy(&hr_disc_uuid, BT_UUID_GATT_CCC, sizeof(hr_disc_uuid));
		hr_disc.uuid = &hr_disc_uuid.uuid;
		hr_disc.start_handle = attr->handle + 2;
		hr_disc.type = BT_GATT_DISCOVER_DESCRIPTOR;
		hr_sub.value_handle = bt_gatt_attr_value_handle(attr);
		bt_gatt_discover(conn, &hr_disc);
	} else {
		hr_sub.notify = hr_notify;
		hr_sub.value = BT_GATT_CCC_NOTIFY;
		hr_sub.ccc_handle = attr->handle;
		bt_gatt_subscribe(conn, &hr_sub);
		return BT_GATT_ITER_STOP;
	}
	return BT_GATT_ITER_STOP;
}

static void hr_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	memcpy(&hr_disc_uuid, BT_UUID_HRS, sizeof(hr_disc_uuid));
	hr_disc.uuid = &hr_disc_uuid.uuid;
	hr_disc.func = hr_discover;
	hr_disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	hr_disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	hr_disc.type = BT_GATT_DISCOVER_PRIMARY;
	bt_gatt_discover(conn, &hr_disc);
}

static void hr_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (conn == hr_conn) {
		bt_conn_unref(hr_conn);
		hr_conn = NULL;
	}
}

BT_CONN_CB_DEFINE(hr_conn_cb) = {
	.connected = hr_connected,
	.disconnected = hr_disconnected,
};

static bool hr_has_service(struct bt_data *data, void *user)
{
	bool *found = user;

	if (data->type == BT_DATA_UUID16_ALL || data->type == BT_DATA_UUID16_SOME) {
		for (int i = 0; i + 1 < data->data_len; i += 2) {
			uint16_t u = sys_get_le16(&data->data[i]);

			if (u == BT_UUID_HRS_VAL) {
				*found = true;
				return false;
			}
		}
	}
	return true;
}

static void hr_scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
		       struct net_buf_simple *ad)
{
	ARG_UNUSED(rssi);
	ARG_UNUSED(type);

	if (!hr_connect_wanted || hr_conn) {
		return;
	}

	bool found = false;

	bt_data_parse(ad, hr_has_service, &found);
	if (!found) {
		return;
	}

	bt_le_scan_stop();

	struct bt_conn_le_create_param *cp = BT_CONN_LE_CREATE_CONN;

	if (bt_conn_le_create(addr, cp, BT_LE_CONN_PARAM_DEFAULT, &hr_conn) != 0) {
		bt_le_scan_start(BT_LE_SCAN_ACTIVE, hr_scan_cb);
	}
}

/* Runs on the BLE worker thread. On success stores the reading in ble_bpm. */
static int ble_do_hr_read(uint32_t timeout_ms)
{
	int rc = ble_ensure_enabled();

	if (rc != 0) {
		return rc;
	}

	k_sem_reset(&hr_done);
	hr_connect_wanted = true;

	rc = bt_le_scan_start(BT_LE_SCAN_ACTIVE, hr_scan_cb);
	if (rc != 0) {
		hr_connect_wanted = false;
		return rc;
	}

	if (k_sem_take(&hr_done, K_MSEC(timeout_ms)) == 0) {
		ble_bpm = hr_value;
		rc = 0;
	} else {
		rc = -ETIMEDOUT;
	}

	hr_connect_wanted = false;
	bt_le_scan_stop();
	if (hr_conn) {
		bt_conn_disconnect(hr_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	return rc;
}

static void ble_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		struct ble_req req;

		if (k_msgq_get(&ble_req_q, &req, K_FOREVER) != 0) {
			continue;
		}

		if (req.op == BLE_OP_SCAN) {
			ble_result = ble_do_scan(req.name, req.timeout_ms);
		} else {
			ble_result = ble_do_hr_read(req.timeout_ms);
		}
		k_sem_give(&ble_done);
	}
}

K_THREAD_DEFINE(app_ble_tid, 2048, ble_thread, NULL, NULL, NULL, 6, 0, 0);

int z_impl_svc_ble_hr_read(uint32_t timeout_ms, uint8_t *out_bpm)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}

	struct ble_req req = { .op = BLE_OP_HR_READ, .timeout_ms = timeout_ms };

	k_mutex_lock(&ble_lock, K_FOREVER);
	(void)k_msgq_put(&ble_req_q, &req, K_FOREVER);
	(void)k_sem_take(&ble_done, K_FOREVER);
	int rc = ble_result;

	if (rc == 0) {
		*out_bpm = ble_bpm;
	}
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

static inline int z_vrfy_svc_ble_hr_read(uint32_t timeout_ms, uint8_t *out_bpm)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out_bpm, 1));
	return z_impl_svc_ble_hr_read(timeout_ms, out_bpm);
}
#include <zephyr/syscalls/svc_ble_hr_read_mrsh.c>
#endif
