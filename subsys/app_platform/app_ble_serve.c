/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The GATT server side of the BLE service. An application describes a service
 * and its characteristics by UUID; the platform builds the attribute table,
 * registers it, advertises, and owns both the connection and the value
 * buffers. The application never sees a bt_conn or an attribute handle, so it
 * cannot reach the stack it is being served by.
 *
 * A peer's write is delivered as an ordinary input event rather than a call
 * into the application: the ATT layer needs a status back synchronously, and a
 * user-mode application cannot be run from that context. The platform stores
 * the value, answers the peer, and posts an event the application picks up on
 * its own thread.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <errno.h>

#include "app_events.h"

#define MAX_APPS   CONFIG_APP_PLATFORM_MAX_APPS
#define NAME_MAX   24
#define VALUE_MAX  APP_BLE_GATT_MAX
#define CHRC_MAX   APP_BLE_SERVE_MAX_CHRC

/* One primary-service attribute plus a declaration and a value per
 * characteristic.
 */
#define ATTR_MAX (1 + (2 * CHRC_MAX))

static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

static K_MUTEX_DEFINE(serve_lock);

/* Only one application serves at a time: the platform has one controller and
 * one advertising set, and a second service would need its own of both.
 */
static int serving_id = -1;
static bool serving;

static union {
	struct bt_uuid_16  u16;
	struct bt_uuid_128 u128;
} svc_uuid_store, chrc_uuid_store[CHRC_MAX];

static struct bt_gatt_attr attrs[ATTR_MAX];
static struct bt_gatt_service svc = {
	.attrs = attrs,
};

static uint8_t values[CHRC_MAX][VALUE_MAX];
static uint16_t value_len[CHRC_MAX];
static uint8_t n_chrc;
static struct bt_gatt_chrc chrc_meta[CHRC_MAX];

/*
 * BT_UUID_GATT_CHRC expands to a compound literal. At file scope that has
 * static storage, but used inside a function it lives only until the block
 * ends, leaving the attribute's uuid pointer dangling. Keep our own copy.
 */
static const struct bt_uuid_16 chrc_decl_uuid = BT_UUID_INIT_16(BT_UUID_GATT_CHRC_VAL);
static const struct bt_uuid_16 primary_decl_uuid = BT_UUID_INIT_16(BT_UUID_GATT_PRIMARY_VAL);
static char adv_name[NAME_MAX];

static struct bt_data ad[2];
static struct bt_conn *serve_conn;

static struct bt_uuid *make_uuid(void *storage, const uint8_t *bytes, size_t len)
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

/* Index of the characteristic a value attribute belongs to. */
static int chrc_index(const struct bt_gatt_attr *attr)
{
	for (int i = 0; i < n_chrc; i++) {
		if (&attrs[2 + (2 * i)] == attr) {
			return i;
		}
	}
	return -1;
}

static ssize_t serve_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	int i = chrc_index(attr);

	if (i < 0) {
		return BT_GATT_ERR(BT_ATT_ERR_ATTRIBUTE_NOT_FOUND);
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, values[i],
				 value_len[i]);
}

static ssize_t serve_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset,
			   uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(flags);

	int i = chrc_index(attr);

	if (i < 0) {
		return BT_GATT_ERR(BT_ATT_ERR_ATTRIBUTE_NOT_FOUND);
	}
	if (offset + len > VALUE_MAX) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(&values[i][offset], buf, len);
	value_len[i] = (uint16_t)(offset + len);

	/* Answer the peer here and hand the application an event: it runs in
	 * user mode on its own thread and cannot be called from this context.
	 */
	struct app_event evt = {
		.kind = APP_EVENT_GATT_WRITE,
		.line = (uint8_t)i,
		.value = (uint8_t)value_len[i],
	};

	app_events_post(serving_id, &evt);
	return len;
}

static void serve_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0U || !serving || serve_conn != NULL) {
		return;
	}
	serve_conn = bt_conn_ref(conn);
}

static void serve_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (conn != serve_conn) {
		return;
	}
	bt_conn_unref(serve_conn);
	serve_conn = NULL;

	/* A peripheral does not resume on its own, and the previous set is
	 * still registered, so stop before starting again.
	 */
	if (serving) {
		(void)bt_le_adv_stop();
		(void)bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad,
				      ARRAY_SIZE(ad), NULL, 0);
	}
}

BT_CONN_CB_DEFINE(app_serve_conn_cb) = {
	.connected = serve_connected,
	.disconnected = serve_disconnected,
};

int z_impl_svc_ble_serve_start(const char *name, const uint8_t *svc_uuid,
			       size_t uuid_len,
			       const struct app_ble_chrc *chrc, size_t n)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}
	if ((uuid_len != 2U && uuid_len != 16U) || n == 0U || n > CHRC_MAX) {
		return -EINVAL;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	if (!app_platform_ble_gatt_granted(id, svc_uuid, uuid_len)) {
		return -EACCES;
	}
	if (k_mutex_lock(&serve_lock, K_SECONDS(2)) != 0) {
		return -EBUSY;
	}
	if (serving) {
		k_mutex_unlock(&serve_lock);
		return -EBUSY;
	}

	int rc = bt_enable(NULL);

	if (rc != 0 && rc != -EALREADY) {
		k_mutex_unlock(&serve_lock);
		return rc;
	}

	(void)memset(attrs, 0, sizeof(attrs));
	(void)memset(values, 0, sizeof(values));
	(void)memset(value_len, 0, sizeof(value_len));
	n_chrc = (uint8_t)n;

	attrs[0] = (struct bt_gatt_attr)BT_GATT_ATTRIBUTE(
		&primary_decl_uuid.uuid, BT_GATT_PERM_READ,
		bt_gatt_attr_read_service, NULL,
		make_uuid(&svc_uuid_store, svc_uuid, uuid_len));

	for (size_t i = 0; i < n; i++) {
		if (chrc[i].uuid_len != 2U && chrc[i].uuid_len != 16U) {
			k_mutex_unlock(&serve_lock);
			return -EINVAL;
		}

		struct bt_uuid *u = make_uuid(&chrc_uuid_store[i],
					      chrc[i].uuid, chrc[i].uuid_len);
		uint8_t props = 0;
		uint8_t perm = 0;

		if (chrc[i].readable != 0U) {
			props |= BT_GATT_CHRC_READ;
			perm |= BT_GATT_PERM_READ;
		}
		if (chrc[i].writable != 0U) {
			props |= BT_GATT_CHRC_WRITE;
			perm |= BT_GATT_PERM_WRITE;
		}

		attrs[1 + (2 * i)] = (struct bt_gatt_attr)
			BT_GATT_ATTRIBUTE(&chrc_decl_uuid.uuid,
					  BT_GATT_PERM_READ,
					  bt_gatt_attr_read_chrc, NULL, NULL);
		attrs[2 + (2 * i)] = (struct bt_gatt_attr)
			BT_GATT_ATTRIBUTE(u, perm, serve_read, serve_write,
					  NULL);

		/* The declaration's user_data describes the value attribute
		 * that follows it, which is what a discovering peer reads. The
		 * value handle is filled in by the host during registration.
		 */
		chrc_meta[i] = (struct bt_gatt_chrc){
			.uuid = u,
			.value_handle = 0U,
			.properties = props,
		};
		attrs[1 + (2 * i)].user_data = &chrc_meta[i];
	}

	svc.attr_count = 1U + (2U * n);
	rc = bt_gatt_service_register(&svc);
	if (rc != 0) {
		k_mutex_unlock(&serve_lock);
		return rc;
	}

	/*
	 * Handles are assigned by the registration, and the value handle a
	 * discovering peer reads comes from the declaration's metadata. The
	 * host only derives it for statically defined services, so fill it in
	 * here: the value attribute is always the one after its declaration.
	 */
	for (size_t i = 0; i < n; i++) {
		chrc_meta[i].value_handle = attrs[2 + (2 * i)].handle;
	}
	(void)strncpy(adv_name, name, sizeof(adv_name) - 1);
	adv_name[sizeof(adv_name) - 1] = '\0';

	static const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;

	ad[0] = (struct bt_data)BT_DATA(BT_DATA_FLAGS, &flags, 1);
	ad[1] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, adv_name,
					(uint8_t)strlen(adv_name));

	rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			     NULL, 0);
	if (rc != 0) {
		(void)bt_gatt_service_unregister(&svc);
		k_mutex_unlock(&serve_lock);
		return rc;
	}

	serving_id = id;
	serving = true;
	k_mutex_unlock(&serve_lock);
	return 0;
}

int z_impl_svc_ble_serve_set(int idx, const uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}
	if (idx < 0 || idx >= (int)n_chrc || len > VALUE_MAX) {
		return -EINVAL;
	}
	if (app_platform_id_current() != serving_id) {
		return -EACCES;
	}

	memcpy(values[idx], buf, len);
	value_len[idx] = (uint16_t)len;
	return 0;
}

int z_impl_svc_ble_serve_get(int idx, uint8_t *buf, size_t cap)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}
	if (idx < 0 || idx >= (int)n_chrc) {
		return -EINVAL;
	}
	if (app_platform_id_current() != serving_id) {
		return -EACCES;
	}

	size_t n = MIN(cap, (size_t)value_len[idx]);

	memcpy(buf, values[idx], n);
	return (int)n;
}

int z_impl_svc_ble_serve_stop(void)
{
	if (!app_has_cap(APP_CAP_BLE)) {
		return -EPERM;
	}
	if (k_mutex_lock(&serve_lock, K_SECONDS(2)) != 0) {
		return -EBUSY;
	}
	if (serving && app_platform_id_current() == serving_id) {
		serving = false;
		(void)bt_le_adv_stop();
		(void)bt_gatt_service_unregister(&svc);
		serving_id = -1;
	}
	k_mutex_unlock(&serve_lock);
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_ble_serve_start(const char *name,
					     const uint8_t *svc_uuid,
					     size_t uuid_len,
					     const struct app_ble_chrc *chrc,
					     size_t n)
{
	char nbuf[NAME_MAX];
	uint8_t ubuf[16];
	struct app_ble_chrc cbuf[CHRC_MAX];

	if ((uuid_len != 2U && uuid_len != 16U) || n == 0U || n > CHRC_MAX) {
		return -EINVAL;
	}
	if (k_usermode_string_copy(nbuf, (char *)name, sizeof(nbuf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(svc_uuid, uuid_len));
	K_OOPS(K_SYSCALL_MEMORY_READ(chrc, n * sizeof(*chrc)));
	memcpy(ubuf, svc_uuid, uuid_len);
	memcpy(cbuf, chrc, n * sizeof(*chrc));

	return z_impl_svc_ble_serve_start(nbuf, ubuf, uuid_len, cbuf, n);
}
#include <zephyr/syscalls/svc_ble_serve_start_mrsh.c>

static inline int z_vrfy_svc_ble_serve_set(int idx, const uint8_t *buf,
					   size_t len)
{
	uint8_t vbuf[VALUE_MAX];

	if (len > VALUE_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	memcpy(vbuf, buf, len);
	return z_impl_svc_ble_serve_set(idx, vbuf, len);
}
#include <zephyr/syscalls/svc_ble_serve_set_mrsh.c>

static inline int z_vrfy_svc_ble_serve_get(int idx, uint8_t *buf, size_t cap)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	return z_impl_svc_ble_serve_get(idx, buf, cap);
}
#include <zephyr/syscalls/svc_ble_serve_get_mrsh.c>

static inline int z_vrfy_svc_ble_serve_stop(void)
{
	return z_impl_svc_ble_serve_stop();
}
#include <zephyr/syscalls/svc_ble_serve_stop_mrsh.c>
#endif
