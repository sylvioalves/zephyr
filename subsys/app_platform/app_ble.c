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
#include "app_grants.h"
#ifdef CONFIG_APP_PLATFORM_BLE_OTA
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/fs/fs.h>
#endif

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

/*
 * Bounded wait for the BLE request lock: a caller waits at most one in-flight
 * operation's own timeout plus this margin (in milliseconds), then gets -EBUSY
 * instead of being parked for ever behind a stuck request.
 */
#define BLE_LOCK_MARGIN_MS 2000U

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
	BLE_OP_OTA_FETCH,
	BLE_OP_GATT_READ,
	BLE_OP_GATT_WRITE,
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
#ifdef CONFIG_APP_PLATFORM_BLE_OTA
/* Bytes of module written by the last completed OTA fetch. */
static size_t ble_ota_len;
#endif

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

	if (app_svc_lock(&ble_lock, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		return -EBUSY;
	}
	(void)k_msgq_put(&ble_req_q, &req, K_NO_WAIT);
	if (k_sem_take(&ble_done, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		app_svc_unlock(&ble_lock);
		return -ETIMEDOUT;
	}
	int rc = ble_result;

	app_svc_unlock(&ble_lock);
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
	/* Connection callbacks are global: ignore a connection this flow did
	 * not open, or the heart-rate discovery would run on top of another
	 * flow's connection and the two would race.
	 */
	if (err != 0U || conn != hr_conn) {
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

#ifdef CONFIG_APP_PLATFORM_BLE_OTA
static int ble_do_ota_fetch(const char *name, uint32_t timeout_ms);
#endif

#ifdef CONFIG_APP_PLATFORM_BLE_GATT
/*
 * The generic GATT client. An application names a service and a characteristic
 * by UUID; the platform connects, discovers both and performs one read or
 * write. Handles never leave the platform, so an application cannot address an
 * attribute it did not name, and a UUID is only accepted when it was granted
 * to that application, which keeps a granted peer from being used as a way to
 * reach every service it happens to expose.
 */
#define GATT_MAX_SVC CONFIG_APP_PLATFORM_BLE_GATT_MAX_SERVICES

struct gatt_grant {
	uint8_t uuid[16];
	uint8_t len;
};

static struct gatt_grant gatt_grants[MAX_APPS][GATT_MAX_SVC];

void app_platform_grant_ble_gatt(int id, const uint8_t *uuid, size_t uuid_len)
{
	if (id < 0 || id >= MAX_APPS || uuid == NULL ||
	    (uuid_len != 2U && uuid_len != 16U)) {
		return;
	}

	for (int i = 0; i < GATT_MAX_SVC; i++) {
		if (gatt_grants[id][i].len == 0U) {
			memcpy(gatt_grants[id][i].uuid, uuid, uuid_len);
			gatt_grants[id][i].len = (uint8_t)uuid_len;
			return;
		}
	}
}

bool app_platform_ble_gatt_granted(int id, const uint8_t *uuid, size_t uuid_len)
{
	if (id < 0 || id >= MAX_APPS) {
		return false;
	}

	for (int i = 0; i < GATT_MAX_SVC; i++) {
		if (gatt_grants[id][i].len == (uint8_t)uuid_len &&
		    memcmp(gatt_grants[id][i].uuid, uuid, uuid_len) == 0) {
			return true;
		}
	}
	return false;
}

/* Request parameters and result, owned by the caller holding ble_lock. */
static uint8_t gatt_svc_uuid_buf[16];
static uint8_t gatt_chr_uuid_buf[16];
static size_t gatt_svc_uuid_len;
static size_t gatt_chr_uuid_len;
static uint8_t gatt_value[APP_BLE_GATT_MAX];
static size_t gatt_value_len;

static struct bt_conn *gatt_conn;
static char gatt_want[BLE_NAME_MAX];
static bool gatt_connect_wanted;
static uint16_t gatt_chr_handle;
static K_SEM_DEFINE(gatt_ready, 0, 1);
static K_SEM_DEFINE(gatt_step, 0, 1);
static int gatt_step_rc;

static union {
	struct bt_uuid_16  u16;
	struct bt_uuid_128 u128;
} gatt_svc_u, gatt_chr_u;

static struct bt_gatt_discover_params gatt_disc;
static struct bt_gatt_read_params gatt_read;
static struct bt_gatt_write_params gatt_write;

/* Build a bt_uuid from the application's bytes. */
static struct bt_uuid *gatt_make_uuid(void *storage, const uint8_t *bytes,
				      size_t len)
{
	if (len == 2U) {
		struct bt_uuid_16 *u = storage;

		u->uuid.type = BT_UUID_TYPE_16;
		u->val = sys_get_le16(bytes);
		return &u->uuid;
	}

	struct bt_uuid_128 *u = storage;

	u->uuid.type = BT_UUID_TYPE_128;
	memcpy(u->val, bytes, 16);
	return &u->uuid;
}

static uint8_t gatt_discover_cb(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				struct bt_gatt_discover_params *params)
{
	if (attr == NULL) {
		/* Discovery ended without the characteristic: release the
		 * waiter so the request fails as -ENOENT instead of timing out.
		 */
		k_sem_give(&gatt_ready);
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		gatt_disc.uuid = gatt_make_uuid(&gatt_chr_u, gatt_chr_uuid_buf,
						gatt_chr_uuid_len);
		gatt_disc.start_handle = attr->handle + 1;
		gatt_disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		bt_gatt_discover(conn, &gatt_disc);
	} else {
		gatt_chr_handle = bt_gatt_attr_value_handle(attr);
		k_sem_give(&gatt_ready);
	}
	return BT_GATT_ITER_STOP;
}

static void gatt_start_discovery(struct bt_conn *conn)
{
	gatt_disc.uuid = gatt_make_uuid(&gatt_svc_u, gatt_svc_uuid_buf,
					gatt_svc_uuid_len);
	gatt_disc.func = gatt_discover_cb;
	gatt_disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	gatt_disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	gatt_disc.type = BT_GATT_DISCOVER_PRIMARY;
	bt_gatt_discover(conn, &gatt_disc);
}

static uint8_t gatt_read_cb(struct bt_conn *conn, uint8_t err,
			    struct bt_gatt_read_params *params,
			    const void *data, uint16_t len)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err != 0U) {
		gatt_step_rc = -EIO;
		k_sem_give(&gatt_step);
		return BT_GATT_ITER_STOP;
	}
	if (data == NULL) {
		gatt_step_rc = 0;
		k_sem_give(&gatt_step);
		return BT_GATT_ITER_STOP;
	}

	size_t n = MIN((size_t)len, sizeof(gatt_value) - gatt_value_len);

	memcpy(&gatt_value[gatt_value_len], data, n);
	gatt_value_len += n;
	return BT_GATT_ITER_CONTINUE;
}

static void gatt_write_cb(struct bt_conn *conn, uint8_t err,
			  struct bt_gatt_write_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);
	gatt_step_rc = (err == 0U) ? 0 : -EIO;
	k_sem_give(&gatt_step);
}

static void gatt_connected(struct bt_conn *conn, uint8_t err)
{
	if (!gatt_connect_wanted || conn != gatt_conn) {
		return;
	}
	if (err != 0U) {
		k_sem_give(&gatt_ready);
		return;
	}
	gatt_chr_handle = 0;
	gatt_start_discovery(conn);
}

static void gatt_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (conn == gatt_conn) {
		bt_conn_unref(gatt_conn);
		gatt_conn = NULL;
	}
}

BT_CONN_CB_DEFINE(app_gatt_conn_cb) = {
	.connected = gatt_connected,
	.disconnected = gatt_disconnected,
};

static bool gatt_name_cb(struct bt_data *data, void *user)
{
	bool *found = user;

	if (data->type == BT_DATA_NAME_COMPLETE ||
	    data->type == BT_DATA_NAME_SHORTENED) {
		size_t len = MIN(data->data_len, (size_t)BLE_NAME_MAX - 1);

		if (strlen(gatt_want) == len &&
		    strncmp((const char *)data->data, gatt_want, len) == 0) {
			*found = true;
			return false;
		}
	}
	return true;
}

static void gatt_scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	ARG_UNUSED(rssi);
	ARG_UNUSED(type);

	if (!gatt_connect_wanted || gatt_conn != NULL) {
		return;
	}

	bool found = false;

	bt_data_parse(ad, gatt_name_cb, &found);
	if (!found) {
		return;
	}
	bt_le_scan_stop();
	(void)bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT, &gatt_conn);
}

/*
 * Connect to the named peer, discover the requested characteristic and perform
 * one exchange. Runs on the BLE worker thread.
 */
static int ble_do_gatt(bool is_write, const char *name, uint32_t timeout_ms)
{
	int rc = ble_ensure_enabled();

	if (rc != 0) {
		return rc;
	}

	strncpy(gatt_want, name, sizeof(gatt_want) - 1);
	gatt_want[sizeof(gatt_want) - 1] = '\0';
	gatt_chr_handle = 0;

	/*
	 * Reuse a link that is already up to the same peer. Each service call
	 * is otherwise its own connection, and a peripheral does not resume
	 * advertising the instant the previous one drops, so reconnecting per
	 * call would lose the peer between two calls of the same application.
	 */
	if (gatt_conn == NULL) {
		gatt_connect_wanted = true;
		k_sem_reset(&gatt_ready);

		struct bt_le_scan_param sp = {
			.type = BT_LE_SCAN_TYPE_ACTIVE,
			.options = BT_LE_SCAN_OPT_NONE,
			.interval = BT_GAP_SCAN_FAST_INTERVAL,
			.window = BT_GAP_SCAN_FAST_WINDOW,
		};

		rc = bt_le_scan_start(&sp, gatt_scan_cb);
		if (rc != 0) {
			goto out;
		}
		if (k_sem_take(&gatt_ready, K_MSEC(timeout_ms)) != 0) {
			rc = -ETIMEDOUT;
			goto out;
		}
	} else {
		/* Already connected: discover this request's characteristic. */
		k_sem_reset(&gatt_ready);
		gatt_start_discovery(gatt_conn);
		if (k_sem_take(&gatt_ready, K_MSEC(timeout_ms)) != 0) {
			rc = -ETIMEDOUT;
			goto out;
		}
	}
	if (gatt_chr_handle == 0U) {
		rc = -ENOENT;
		goto out;
	}

	k_sem_reset(&gatt_step);
	if (is_write) {
		gatt_write.handle = gatt_chr_handle;
		gatt_write.offset = 0;
		gatt_write.data = gatt_value;
		gatt_write.length = (uint16_t)gatt_value_len;
		gatt_write.func = gatt_write_cb;
		rc = bt_gatt_write(gatt_conn, &gatt_write);
	} else {
		gatt_value_len = 0;
		gatt_read.func = gatt_read_cb;
		gatt_read.handle_count = 1;
		gatt_read.single.handle = gatt_chr_handle;
		gatt_read.single.offset = 0;
		rc = bt_gatt_read(gatt_conn, &gatt_read);
	}
	if (rc != 0) {
		goto out;
	}
	if (k_sem_take(&gatt_step, K_MSEC(timeout_ms)) != 0) {
		rc = -ETIMEDOUT;
		goto out;
	}
	rc = (gatt_step_rc != 0) ? gatt_step_rc : (int)gatt_value_len;
out:
	gatt_connect_wanted = false;
	bt_le_scan_stop();
	return rc;
}

#endif /* CONFIG_APP_PLATFORM_BLE_GATT */

static void ble_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		struct ble_req req;

		if (k_msgq_get(&ble_req_q, &req, K_FOREVER) != 0) {
			continue;
		}

		switch (req.op) {
		case BLE_OP_SCAN:
			ble_result = ble_do_scan(req.name, req.timeout_ms);
			break;
#ifdef CONFIG_APP_PLATFORM_BLE_OTA
		case BLE_OP_OTA_FETCH:
			ble_result = ble_do_ota_fetch(req.name, req.timeout_ms);
			break;
#endif
#ifdef CONFIG_APP_PLATFORM_BLE_GATT
		case BLE_OP_GATT_READ:
		case BLE_OP_GATT_WRITE:
			ble_result = ble_do_gatt(req.op == BLE_OP_GATT_WRITE,
						 req.name, req.timeout_ms);
			break;
#endif
		default:
			ble_result = ble_do_hr_read(req.timeout_ms);
			break;
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

	if (app_svc_lock(&ble_lock, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		return -EBUSY;
	}
	(void)k_msgq_put(&ble_req_q, &req, K_NO_WAIT);
	if (k_sem_take(&ble_done, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		app_svc_unlock(&ble_lock);
		return -ETIMEDOUT;
	}
	int rc = ble_result;
	uint8_t bpm = ble_bpm;

	app_svc_unlock(&ble_lock);
	if (rc == 0) {
		*out_bpm = bpm;
	}
	return rc;
}

#ifdef CONFIG_APP_PLATFORM_BLE_OTA
/*
 * Pull a signed application from a BLE peer.
 *
 * The peer advertises the OTA service and exposes two characteristics: a
 * control point the device writes a 4-byte little-endian offset to, and a data
 * characteristic it then reads one chunk from. Reading by offset rather than
 * taking a push stream keeps the flow under the device's control: the next
 * request is only issued once the previous chunk is on flash, so a slow write
 * cannot drop data and no acknowledgment scheme is needed.
 *
 * The transfer is untrusted. Its output is a manifest, a signature and the
 * module bytes on flash; nothing is launched here. The caller verifies the
 * signature and the module hash exactly as it would for an HTTP download, so a
 * peer that lies about either is rejected by the same check.
 */
#define OTA_HDR_LEN  (sizeof(struct app_manifest) + APP_MANIFEST_SIG_LEN)
#define OTA_CHUNK_MAX 244U

static struct bt_conn *ota_conn;
static struct bt_uuid_128 ota_disc_uuid = BT_UUID_INIT_128(0);
static struct bt_gatt_discover_params ota_disc;
static struct bt_gatt_read_params ota_read;
static struct bt_gatt_exchange_params ota_mtu;
static struct bt_gatt_write_params ota_write;
static K_SEM_DEFINE(ota_ready, 0, 1);
static K_SEM_DEFINE(ota_step, 0, 1);
static uint16_t ota_ctrl_handle;
static uint16_t ota_data_handle;
static char ota_want[BLE_NAME_MAX];
static bool ota_connect_wanted;
static uint8_t ota_chunk[OTA_CHUNK_MAX];
static uint16_t ota_chunk_len;
static int ota_step_rc;

static const struct bt_uuid_128 ota_svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3d0, 0x1111, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 ota_ctrl_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3d1, 0x1111, 0x4a5b, 0x8c7d, 0x0123456789ab));
static const struct bt_uuid_128 ota_data_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xa1b2c3d2, 0x1111, 0x4a5b, 0x8c7d, 0x0123456789ab));

static uint8_t ota_read_cb(struct bt_conn *conn, uint8_t err,
			   struct bt_gatt_read_params *params, const void *data,
			   uint16_t len)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err != 0U) {
		ota_step_rc = -EIO;
		ota_chunk_len = 0;
		k_sem_give(&ota_step);
		return BT_GATT_ITER_STOP;
	}
	if (data == NULL) {
		/* End of this read: whatever was copied is the whole chunk. */
		ota_step_rc = 0;
		k_sem_give(&ota_step);
		return BT_GATT_ITER_STOP;
	}
	uint16_t n = MIN(len, (uint16_t)(sizeof(ota_chunk) - ota_chunk_len));

	memcpy(&ota_chunk[ota_chunk_len], data, n);
	ota_chunk_len += n;
	return BT_GATT_ITER_CONTINUE;
}

static void ota_write_cb(struct bt_conn *conn, uint8_t err,
			 struct bt_gatt_write_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);
	ota_step_rc = (err == 0U) ? 0 : -EIO;
	k_sem_give(&ota_step);
}

/* Ask the peer for the bytes at @offset, and block until they arrive. */
static int ota_read_at(uint32_t offset, uint32_t timeout_ms)
{
	uint8_t off[4];

	sys_put_le32(offset, off);
	ota_write.handle = ota_ctrl_handle;
	ota_write.offset = 0;
	ota_write.data = off;
	ota_write.length = sizeof(off);
	ota_write.func = ota_write_cb;

	k_sem_reset(&ota_step);
	int rc = bt_gatt_write(ota_conn, &ota_write);

	if (rc != 0) {
		return rc;
	}
	if (k_sem_take(&ota_step, K_MSEC(timeout_ms)) != 0) {
		return -ETIMEDOUT;
	}
	if (ota_step_rc != 0) {
		return ota_step_rc;
	}

	ota_chunk_len = 0;
	ota_read.func = ota_read_cb;
	/* A read returns at most MTU-1 bytes; asking for more just means more
	 * round trips, so the peer is told the offset and hands back whatever
	 * one exchange can carry.
	 */
	ota_read.handle_count = 1;
	ota_read.single.handle = ota_data_handle;
	ota_read.single.offset = 0;

	k_sem_reset(&ota_step);
	rc = bt_gatt_read(ota_conn, &ota_read);
	if (rc != 0) {
		return rc;
	}
	if (k_sem_take(&ota_step, K_MSEC(timeout_ms)) != 0) {
		return -ETIMEDOUT;
	}
	return ota_step_rc;
}

static uint8_t ota_discover(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    struct bt_gatt_discover_params *params)
{
	if (attr == NULL) {
		/* Discovery finished; ota_ready is only given once both
		 * handles were found, so a partial peer fails the wait.
		 */
		return BT_GATT_ITER_STOP;
	}

	if (bt_uuid_cmp(params->uuid, &ota_svc_uuid.uuid) == 0) {
		memcpy(&ota_disc_uuid, &ota_ctrl_uuid, sizeof(ota_disc_uuid));
		ota_disc.uuid = &ota_disc_uuid.uuid;
		ota_disc.start_handle = attr->handle + 1;
		ota_disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		bt_gatt_discover(conn, &ota_disc);
	} else if (bt_uuid_cmp(params->uuid, &ota_ctrl_uuid.uuid) == 0) {
		ota_ctrl_handle = bt_gatt_attr_value_handle(attr);
		memcpy(&ota_disc_uuid, &ota_data_uuid, sizeof(ota_disc_uuid));
		ota_disc.uuid = &ota_disc_uuid.uuid;
		ota_disc.start_handle = attr->handle + 2;
		ota_disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
		bt_gatt_discover(conn, &ota_disc);
	} else {
		ota_data_handle = bt_gatt_attr_value_handle(attr);
		if (ota_ctrl_handle != 0U && ota_data_handle != 0U) {
			k_sem_give(&ota_ready);
		}
	}
	return BT_GATT_ITER_STOP;
}

static void ota_mtu_cb(struct bt_conn *conn, uint8_t err,
		       struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);
	/* Discovery starts once the MTU is settled either way: a peer that
	 * refuses the exchange simply keeps the 23-byte default, which is slow
	 * but still correct.
	 */
	if (err != 0U) {
		printk("ble-ota: MTU exchange refused, staying at the default\n");
	}
	memcpy(&ota_disc_uuid, &ota_svc_uuid, sizeof(ota_disc_uuid));
	ota_disc.uuid = &ota_disc_uuid.uuid;
	ota_disc.func = ota_discover;
	ota_disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	ota_disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	ota_disc.type = BT_GATT_DISCOVER_PRIMARY;
	bt_gatt_discover(conn, &ota_disc);
}

static void ota_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U || conn != ota_conn) {
		return;
	}
	ota_ctrl_handle = 0;
	ota_data_handle = 0;

	/*
	 * Raise the ATT MTU before discovering anything. At the 23-byte default
	 * a read returns about 22 bytes, so a module of a few kilobytes costs
	 * hundreds of round trips; one exchange makes each read carry an entire
	 * chunk instead.
	 */
	ota_mtu.func = ota_mtu_cb;
	if (bt_gatt_exchange_mtu(conn, &ota_mtu) == 0) {
		return;
	}
	memcpy(&ota_disc_uuid, &ota_svc_uuid, sizeof(ota_disc_uuid));
	ota_disc.uuid = &ota_disc_uuid.uuid;
	ota_disc.func = ota_discover;
	ota_disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	ota_disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	ota_disc.type = BT_GATT_DISCOVER_PRIMARY;
	bt_gatt_discover(conn, &ota_disc);
}

static void ota_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (conn == ota_conn) {
		bt_conn_unref(ota_conn);
		ota_conn = NULL;
	}
}

BT_CONN_CB_DEFINE(ota_conn_cb) = {
	.connected = ota_connected,
	.disconnected = ota_disconnected,
};

static bool ota_name_cb(struct bt_data *data, void *user)
{
	bool *found = user;

	if (data->type == BT_DATA_NAME_COMPLETE ||
	    data->type == BT_DATA_NAME_SHORTENED) {
		size_t len = MIN(data->data_len, (size_t)BLE_NAME_MAX - 1);

		if (strlen(ota_want) == len &&
		    strncmp((const char *)data->data, ota_want, len) == 0) {
			*found = true;
			return false;
		}
	}
	return true;
}

static void ota_scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			struct net_buf_simple *ad)
{
	ARG_UNUSED(rssi);
	ARG_UNUSED(type);

	if (!ota_connect_wanted || ota_conn != NULL) {
		return;
	}

	bool found = false;

	bt_data_parse(ad, ota_name_cb, &found);
	if (!found) {
		return;
	}
	bt_le_scan_stop();
	(void)bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT, &ota_conn);
}

/*
 * Fetch the named application from the peer and leave it on flash. Runs on the
 * BLE worker thread; the calling application is parked in the syscall.
 */
static int ble_do_ota_fetch(const char *name, uint32_t timeout_ms)
{
	int rc = ble_ensure_enabled();

	if (rc != 0) {
		return rc;
	}

	strncpy(ota_want, name, sizeof(ota_want) - 1);
	ota_want[sizeof(ota_want) - 1] = '\0';
	ota_connect_wanted = true;
	ota_ctrl_handle = 0;
	ota_data_handle = 0;
	ble_ota_len = 0;
	k_sem_reset(&ota_ready);

	struct bt_le_scan_param sp = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	rc = bt_le_scan_start(&sp, ota_scan_cb);
	if (rc != 0) {
		goto out;
	}
	if (k_sem_take(&ota_ready, K_MSEC(timeout_ms)) != 0) {
		rc = -ETIMEDOUT;
		goto out;
	}

	/* Header first: the manifest and its signature, at offset zero. */
	rc = ota_read_at(0, timeout_ms);
	if (rc != 0) {
		goto out;
	}
	if (ota_chunk_len < OTA_HDR_LEN) {
		rc = -EBADMSG;
		goto out;
	}

	struct app_manifest mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];

	memcpy(&mf, ota_chunk, sizeof(mf));
	memcpy(sig, &ota_chunk[sizeof(mf)], sizeof(sig));

	if (mf.magic != APP_MANIFEST_MAGIC) {
		rc = -EBADMSG;
		goto out;
	}
	if (mf.module_len == 0U ||
	    mf.module_len > CONFIG_APP_PLATFORM_LOADER_MAX_MODULE_BYTES) {
		rc = -EFBIG;
		goto out;
	}
	/* The peer names the application; the manifest is what decides it. */
	if (strncmp(mf.name, name, APP_MANIFEST_NAME_MAX) != 0) {
		rc = -EPERM;
		goto out;
	}

	rc = app_registry_store_staged_manifest(name, &mf, sig, sizeof(sig));
	if (rc != 0) {
		goto out;
	}

	/* Then the module, one chunk at a time, to the staged path. */
	char path[64];

	rc = app_registry_staged_blob_path(name, path, sizeof(path));
	if (rc != 0) {
		goto out;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		goto out;
	}

	uint32_t got = 0;

	while (got < mf.module_len) {
		rc = ota_read_at(OTA_HDR_LEN + got, timeout_ms);
		if (rc != 0) {
			break;
		}
		if (ota_chunk_len == 0U) {
			/* Peer ran out of bytes before the manifest said so. */
			rc = -EBADMSG;
			break;
		}
		uint32_t want = MIN((uint32_t)ota_chunk_len,
				    mf.module_len - got);
		ssize_t n = fs_write(&f, ota_chunk, want);

		if (n < 0 || (uint32_t)n != want) {
			rc = -EIO;
			break;
		}
		got += want;
	}
	fs_close(&f);

	if (rc == 0 && got != mf.module_len) {
		rc = -EBADMSG;
	}
	if (rc == 0) {
		ble_ota_len = got;
	}

out:
	ota_connect_wanted = false;
	bt_le_scan_stop();
	if (ota_conn != NULL) {
		bt_conn_disconnect(ota_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	return rc;
}

int z_impl_svc_ble_ota_fetch(const char *name, uint32_t timeout_ms,
			     uint32_t *out_len)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}

	struct ble_req req = { .op = BLE_OP_OTA_FETCH, .timeout_ms = timeout_ms };

	strncpy(req.name, name, sizeof(req.name) - 1);
	req.name[sizeof(req.name) - 1] = '\0';

	if (app_svc_lock(&ble_lock, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		return -EBUSY;
	}
	(void)k_msgq_put(&ble_req_q, &req, K_NO_WAIT);
	if (k_sem_take(&ble_done, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		app_svc_unlock(&ble_lock);
		return -ETIMEDOUT;
	}
	int rc = ble_result;
	uint32_t len = (uint32_t)ble_ota_len;

	app_svc_unlock(&ble_lock);
	if (rc == 0 && out_len != NULL) {
		*out_len = len;
	}
	return rc;
}
#endif /* CONFIG_APP_PLATFORM_BLE_OTA */

#ifdef CONFIG_APP_PLATFORM_BLE_GATT
/*
 * Shared entry for both directions. The capability and the service grant are
 * checked before anything is copied or the worker is woken, so an ungranted
 * caller never reaches the controller.
 */
static int gatt_request(bool is_write, const char *name,
			const uint8_t *svc_uuid, size_t svc_uuid_len,
			const uint8_t *chr_uuid, size_t chr_uuid_len,
			uint8_t *buf, size_t len, uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}
	if ((svc_uuid_len != 2U && svc_uuid_len != 16U) ||
	    (chr_uuid_len != 2U && chr_uuid_len != 16U) ||
	    len > APP_BLE_GATT_MAX) {
		return -EINVAL;
	}
	if (!app_platform_ble_gatt_granted(app_platform_id_current(), svc_uuid, svc_uuid_len)) {
		return -EACCES;
	}

	struct ble_req req = {
		.op = is_write ? BLE_OP_GATT_WRITE : BLE_OP_GATT_READ,
		.timeout_ms = timeout_ms,
	};

	strncpy(req.name, name, sizeof(req.name) - 1);
	req.name[sizeof(req.name) - 1] = '\0';

	if (app_svc_lock(&ble_lock, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		return -EBUSY;
	}

	memcpy(gatt_svc_uuid_buf, svc_uuid, svc_uuid_len);
	memcpy(gatt_chr_uuid_buf, chr_uuid, chr_uuid_len);
	gatt_svc_uuid_len = svc_uuid_len;
	gatt_chr_uuid_len = chr_uuid_len;
	if (is_write) {
		memcpy(gatt_value, buf, len);
		gatt_value_len = len;
	}

	(void)k_msgq_put(&ble_req_q, &req, K_NO_WAIT);
	if (k_sem_take(&ble_done, K_MSEC(timeout_ms + BLE_LOCK_MARGIN_MS)) != 0) {
		app_svc_unlock(&ble_lock);
		return -ETIMEDOUT;
	}

	int rc = ble_result;

	if (!is_write && rc > 0) {
		size_t n = MIN((size_t)rc, len);

		memcpy(buf, gatt_value, n);
		rc = (int)n;
	}
	app_svc_unlock(&ble_lock);
	return rc;
}

int z_impl_svc_ble_gatt_read(const char *name,
			     const uint8_t *svc_uuid, size_t svc_uuid_len,
			     const uint8_t *chr_uuid, size_t chr_uuid_len,
			     uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
	return gatt_request(false, name, svc_uuid, svc_uuid_len, chr_uuid,
			    chr_uuid_len, buf, cap, timeout_ms);
}

int z_impl_svc_ble_gatt_write(const char *name,
			      const uint8_t *svc_uuid, size_t svc_uuid_len,
			      const uint8_t *chr_uuid, size_t chr_uuid_len,
			      const uint8_t *buf, size_t len,
			      uint32_t timeout_ms)
{
	int rc = gatt_request(true, name, svc_uuid, svc_uuid_len, chr_uuid,
			      chr_uuid_len, (uint8_t *)buf, len, timeout_ms);

	return (rc > 0) ? 0 : rc;
}
#endif /* CONFIG_APP_PLATFORM_BLE_GATT */

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

#ifdef CONFIG_APP_PLATFORM_BLE_OTA
static inline int z_vrfy_svc_ble_ota_fetch(const char *name,
					   uint32_t timeout_ms,
					   uint32_t *out_len)
{
	char buf[BLE_NAME_MAX];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	if (out_len != NULL) {
		K_OOPS(K_SYSCALL_MEMORY_WRITE(out_len, sizeof(*out_len)));
	}
	return z_impl_svc_ble_ota_fetch(buf, timeout_ms, out_len);
}
#include <zephyr/syscalls/svc_ble_ota_fetch_mrsh.c>
#endif
#ifdef CONFIG_APP_PLATFORM_BLE_GATT
/*
 * Both UUIDs and the value are copied into platform memory before use: the
 * lengths are validated first so a caller cannot describe a buffer larger than
 * it owns, and the copies mean a second thread in the same application cannot
 * change the bytes after they were checked.
 */
static inline int z_vrfy_svc_ble_gatt_read(const char *name,
					   const uint8_t *svc_uuid,
					   size_t svc_uuid_len,
					   const uint8_t *chr_uuid,
					   size_t chr_uuid_len,
					   uint8_t *buf, size_t cap,
					   uint32_t timeout_ms)
{
	char nbuf[BLE_NAME_MAX];
	uint8_t svc[16];
	uint8_t chr[16];
	uint8_t vbuf[APP_BLE_GATT_MAX];
	int rc;

	if ((svc_uuid_len != 2U && svc_uuid_len != 16U) ||
	    (chr_uuid_len != 2U && chr_uuid_len != 16U) ||
	    cap > APP_BLE_GATT_MAX) {
		return -EINVAL;
	}
	if (k_usermode_string_copy(nbuf, (char *)name, sizeof(nbuf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(svc_uuid, svc_uuid_len));
	K_OOPS(K_SYSCALL_MEMORY_READ(chr_uuid, chr_uuid_len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	memcpy(svc, svc_uuid, svc_uuid_len);
	memcpy(chr, chr_uuid, chr_uuid_len);

	rc = z_impl_svc_ble_gatt_read(nbuf, svc, svc_uuid_len, chr,
				      chr_uuid_len, vbuf, cap, timeout_ms);
	if (rc > 0) {
		K_OOPS(k_usermode_to_copy(buf, vbuf, rc));
	}
	return rc;
}
#include <zephyr/syscalls/svc_ble_gatt_read_mrsh.c>

static inline int z_vrfy_svc_ble_gatt_write(const char *name,
					    const uint8_t *svc_uuid,
					    size_t svc_uuid_len,
					    const uint8_t *chr_uuid,
					    size_t chr_uuid_len,
					    const uint8_t *buf, size_t len,
					    uint32_t timeout_ms)
{
	char nbuf[BLE_NAME_MAX];
	uint8_t svc[16];
	uint8_t chr[16];
	uint8_t vbuf[APP_BLE_GATT_MAX];

	if ((svc_uuid_len != 2U && svc_uuid_len != 16U) ||
	    (chr_uuid_len != 2U && chr_uuid_len != 16U) ||
	    len > APP_BLE_GATT_MAX) {
		return -EINVAL;
	}
	if (k_usermode_string_copy(nbuf, (char *)name, sizeof(nbuf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(svc_uuid, svc_uuid_len));
	K_OOPS(K_SYSCALL_MEMORY_READ(chr_uuid, chr_uuid_len));
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	memcpy(svc, svc_uuid, svc_uuid_len);
	memcpy(chr, chr_uuid, chr_uuid_len);
	memcpy(vbuf, buf, len);

	return z_impl_svc_ble_gatt_write(nbuf, svc, svc_uuid_len, chr,
					 chr_uuid_len, vbuf, len, timeout_ms);
}
#include <zephyr/syscalls/svc_ble_gatt_write_mrsh.c>
#endif

#endif
