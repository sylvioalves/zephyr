/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The shared service library. Each service runs in the supervisor, enforces its
 * capability bit at the syscall boundary, and is safe to call concurrently from
 * several isolated applications. This first group (log, crypto, random, time) is
 * stateless beyond the cap check, so concurrency needs no locking.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/random/random.h>
#ifdef CONFIG_PSA_CRYPTO
#include <psa/crypto.h>
#endif
#ifdef CONFIG_SENSOR
#include <zephyr/drivers/sensor.h>
#endif
#ifdef CONFIG_FLASH_MAP
#include <zephyr/storage/flash_map.h>
#endif
#ifdef CONFIG_HWINFO
#include <zephyr/drivers/hwinfo.h>
#endif
#include <string.h>
#include <errno.h>

#define SVC_XFER_MAX 256

static K_MUTEX_DEFINE(flash_lock);

/* A shared, lock-serialized key/value store namespaced by application id. */
#define KV_SLOTS    16
#define KV_KEY_MAX  20

struct kv_entry {
	int      owner;            /* app group id that owns this key */
	char     key[KV_KEY_MAX];
	uint32_t value;
	bool     used;
};
static struct kv_entry kv_store[KV_SLOTS];
static K_MUTEX_DEFINE(kv_lock);

#if defined(CONFIG_SENSOR) && DT_HAS_ALIAS(die_temp0)
static const struct device *const svc_die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));
#endif

/* Per-application mailbox: a fixed-size message queue, one per group. */
#define MAX_APPS  CONFIG_APP_PLATFORM_MAX_APPS
#define IPC_MSG_MAX 64

struct ipc_msg {
	uint16_t len;
	uint8_t  data[IPC_MSG_MAX];
};

K_MSGQ_DEFINE(ipc_q0, sizeof(struct ipc_msg), 4, 4);
K_MSGQ_DEFINE(ipc_q1, sizeof(struct ipc_msg), 4, 4);
K_MSGQ_DEFINE(ipc_q2, sizeof(struct ipc_msg), 4, 4);
K_MSGQ_DEFINE(ipc_q3, sizeof(struct ipc_msg), 4, 4);
static struct k_msgq *const mailbox[] = { &ipc_q0, &ipc_q1, &ipc_q2, &ipc_q3 };

BUILD_ASSERT(MAX_APPS <= ARRAY_SIZE(mailbox),
	     "add a mailbox per supported application group");

/* True when the calling application holds every bit in @mask. */
static inline bool app_has_cap(uint32_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

void z_impl_svc_heartbeat(void)
{
	app_platform_heartbeat();
}

int z_impl_svc_sha256(const uint8_t *in, size_t len, uint8_t *out32)
{
	if (!app_has_cap(APP_CAP_CRYPTO)) {
		return -EPERM;
	}
	if (len > SVC_XFER_MAX) {
		return -EINVAL;
	}
#ifdef CONFIG_PSA_CRYPTO
	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, len, out32, 32,
					   &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
#else
	ARG_UNUSED(out32);
	return -ENOTSUP;
#endif
}

int z_impl_svc_random(uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_RANDOM)) {
		return -EPERM;
	}
	if (len > SVC_XFER_MAX) {
		return -EINVAL;
	}
	sys_rand_get(buf, len);
	return 0;
}

uint32_t z_impl_svc_uptime_ms(void)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return 0;
	}
	return (uint32_t)k_uptime_get();
}

uint32_t z_impl_svc_cycles(void)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return 0;
	}
	return k_cycle_get_32();
}

int z_impl_svc_die_temp_milli(void)
{
	if (!app_has_cap(APP_CAP_SENSOR)) {
		return -EPERM;
	}
#if defined(CONFIG_SENSOR) && DT_HAS_ALIAS(die_temp0)
	if (!device_is_ready(svc_die_temp) ||
	    sensor_sample_fetch(svc_die_temp) != 0) {
		return -EIO;
	}
	struct sensor_value v;

	sensor_channel_get(svc_die_temp, SENSOR_CHAN_DIE_TEMP, &v);
	return v.val1 * 1000 + v.val2 / 1000;
#else
	return -ENOTSUP;
#endif
}

int z_impl_svc_kv_set(const char *key, uint32_t value)
{
	if (!app_has_cap(APP_CAP_KV)) {
		return -EPERM;
	}

	int owner = app_platform_id_current();
	struct kv_entry *slot = NULL;
	int rc = -ENOSPC;

	k_mutex_lock(&kv_lock, K_FOREVER);
	for (int i = 0; i < KV_SLOTS; i++) {
		if (kv_store[i].used && kv_store[i].owner == owner &&
		    strcmp(kv_store[i].key, key) == 0) {
			kv_store[i].value = value;
			rc = 0;
			goto out;
		}
		if (!kv_store[i].used && slot == NULL) {
			slot = &kv_store[i];
		}
	}
	if (slot != NULL) {
		slot->owner = owner;
		strncpy(slot->key, key, sizeof(slot->key) - 1);
		slot->key[sizeof(slot->key) - 1] = '\0';
		slot->value = value;
		slot->used = true;
		rc = 0;
	}
out:
	k_mutex_unlock(&kv_lock);
	return rc;
}

int z_impl_svc_kv_get(const char *key, uint32_t *out)
{
	if (!app_has_cap(APP_CAP_KV)) {
		return -EPERM;
	}

	int owner = app_platform_id_current();
	int rc = -ENOENT;

	k_mutex_lock(&kv_lock, K_FOREVER);
	for (int i = 0; i < KV_SLOTS; i++) {
		if (kv_store[i].used && kv_store[i].owner == owner &&
		    strcmp(kv_store[i].key, key) == 0) {
			*out = kv_store[i].value;
			rc = 0;
			break;
		}
	}
	k_mutex_unlock(&kv_lock);
	return rc;
}

int z_impl_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_FLASH)) {
		return -EPERM;
	}
	if (len > SVC_XFER_MAX) {
		return -EINVAL;
	}
#if defined(CONFIG_FLASH_MAP) && DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
	const struct flash_area *fa;

	if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) != 0) {
		return -EIO;
	}

	int rc = -EINVAL;

	if ((off + len) <= fa->fa_size) {
		k_mutex_lock(&flash_lock, K_FOREVER);
		rc = flash_area_read(fa, off, buf, len);
		k_mutex_unlock(&flash_lock);
	}
	flash_area_close(fa);
	return rc;
#else
	ARG_UNUSED(off);
	ARG_UNUSED(buf);
	return -ENOTSUP;
#endif
}

int z_impl_svc_flash_write(uint32_t off, const uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_FLASH)) {
		return -EPERM;
	}
	if (len > SVC_XFER_MAX) {
		return -EINVAL;
	}
#if defined(CONFIG_FLASH_MAP) && DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
	const struct flash_area *fa;

	if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) != 0) {
		return -EIO;
	}

	int rc = -EINVAL;

	if ((off + len) <= fa->fa_size) {
		k_mutex_lock(&flash_lock, K_FOREVER);
		rc = flash_area_write(fa, off, buf, len);
		k_mutex_unlock(&flash_lock);
		if (rc == 0) {
			rc = (int)len;
		}
	}
	flash_area_close(fa);
	return rc;
#else
	ARG_UNUSED(off);
	ARG_UNUSED(buf);
	return -ENOTSUP;
#endif
}

int z_impl_svc_flash_erase(uint32_t off, size_t len)
{
	if (!app_has_cap(APP_CAP_FLASH)) {
		return -EPERM;
	}
#if defined(CONFIG_FLASH_MAP) && DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
	const struct flash_area *fa;

	if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) != 0) {
		return -EIO;
	}

	int rc = -EINVAL;

	/* Erase is bounded only by the partition; it spans whole sectors. */
	if ((off + len) <= fa->fa_size) {
		k_mutex_lock(&flash_lock, K_FOREVER);
		rc = flash_area_erase(fa, off, len);
		k_mutex_unlock(&flash_lock);
	}
	flash_area_close(fa);
	return rc;
#else
	ARG_UNUSED(off);
	ARG_UNUSED(len);
	return -ENOTSUP;
#endif
}

int z_impl_svc_device_id(uint8_t *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}
	if (len > SVC_XFER_MAX) {
		return -EINVAL;
	}
#ifdef CONFIG_HWINFO
	return (int)hwinfo_get_device_id(buf, len);
#else
	ARG_UNUSED(buf);
	return -ENOTSUP;
#endif
}

int z_impl_svc_reset_cause(uint32_t *cause)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}
#ifdef CONFIG_HWINFO
	return hwinfo_get_reset_cause(cause);
#else
	*cause = 0;
	return -ENOTSUP;
#endif
}

/* Per-application one-shot software timers, fired through a semaphore. */
static struct k_timer app_timers[MAX_APPS];
static struct k_sem timer_sems[MAX_APPS];
static bool timers_inited;

static void app_timer_expiry(struct k_timer *t)
{
	int id = (int)(t - app_timers);

	if (id >= 0 && id < MAX_APPS) {
		k_sem_give(&timer_sems[id]);
	}
}

static void timers_init_once(void)
{
	if (timers_inited) {
		return;
	}
	for (int i = 0; i < MAX_APPS; i++) {
		k_timer_init(&app_timers[i], app_timer_expiry, NULL);
		k_sem_init(&timer_sems[i], 0, 1);
	}
	timers_inited = true;
}

int z_impl_svc_timer_start(uint32_t ms)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	timers_init_once();
	k_sem_reset(&timer_sems[id]);
	k_timer_start(&app_timers[id], K_MSEC(ms), K_NO_WAIT);
	return 0;
}

int z_impl_svc_timer_wait(uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	timers_init_once();
	return k_sem_take(&timer_sems[id], K_MSEC(timeout_ms)) == 0 ? 0 : -EAGAIN;
}

/* A monotonic wall clock: a seed plus elapsed uptime. */
static K_MUTEX_DEFINE(rtc_lock);
static int64_t rtc_seed_sec;
static int64_t rtc_seed_uptime;

int z_impl_svc_rtc_set(uint32_t epoch_sec)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}
	k_mutex_lock(&rtc_lock, K_FOREVER);
	rtc_seed_sec = epoch_sec;
	rtc_seed_uptime = k_uptime_get();
	k_mutex_unlock(&rtc_lock);
	return 0;
}

int z_impl_svc_rtc_now(uint32_t *epoch_sec)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}
	k_mutex_lock(&rtc_lock, K_FOREVER);
	int64_t now = rtc_seed_sec + (k_uptime_get() - rtc_seed_uptime) / 1000;

	k_mutex_unlock(&rtc_lock);
	*epoch_sec = (uint32_t)now;
	return 0;
}

int z_impl_svc_ipc_send(int dest_id, const void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_IPC)) {
		return -EPERM;
	}
	if (dest_id < 0 || dest_id >= MAX_APPS || len > IPC_MSG_MAX) {
		return -EINVAL;
	}

	struct ipc_msg msg;

	msg.len = (uint16_t)len;
	memcpy(msg.data, buf, len);

	return k_msgq_put(mailbox[dest_id], &msg, K_NO_WAIT) == 0 ? 0 : -EAGAIN;
}

int z_impl_svc_ipc_recv(void *buf, size_t len, uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_IPC)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}

	struct ipc_msg msg;

	if (k_msgq_get(mailbox[id], &msg, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
	}

	size_t n = MIN(len, (size_t)msg.len);

	memcpy(buf, msg.data, n);
	return (int)n;
}

int z_impl_svc_app_count(void)
{
	if (!app_has_cap(APP_CAP_INSPECT)) {
		return -EPERM;
	}
	return app_platform_count();
}

int z_impl_svc_app_info(int id, struct svc_app_info *out)
{
	if (!app_has_cap(APP_CAP_INSPECT)) {
		return -EPERM;
	}

	struct app_platform_status st;
	int rc = app_platform_status(id, &st);

	if (rc != 0) {
		return rc;
	}

	*out = (struct svc_app_info){
		.active = st.active,
		.monitored = st.monitored,
		.faulted = st.faulted,
		.stack_size = st.stack_size,
		.stack_used = st.stack_used,
		.cpu_cycles = st.cpu_cycles,
		.cpu_total = st.cpu_total,
	};
	strncpy(out->name, st.name ? st.name : "", sizeof(out->name) - 1);
	out->name[sizeof(out->name) - 1] = '\0';
	return 0;
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_svc_log(const char *msg)
{
	char buf[64];

	if (k_usermode_string_copy(buf, (char *)msg, sizeof(buf)) == 0) {
		z_impl_svc_log(buf);
	}
}
#include <zephyr/syscalls/svc_log_mrsh.c>

static inline void z_vrfy_svc_heartbeat(void)
{
	z_impl_svc_heartbeat();
}
#include <zephyr/syscalls/svc_heartbeat_mrsh.c>

static inline int z_vrfy_svc_sha256(const uint8_t *in, size_t len,
				    uint8_t *out32)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(in, len));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out32, 32));
	return z_impl_svc_sha256(in, len, out32);
}
#include <zephyr/syscalls/svc_sha256_mrsh.c>

static inline int z_vrfy_svc_random(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_random(buf, len);
}
#include <zephyr/syscalls/svc_random_mrsh.c>

static inline uint32_t z_vrfy_svc_uptime_ms(void)
{
	return z_impl_svc_uptime_ms();
}
#include <zephyr/syscalls/svc_uptime_ms_mrsh.c>

static inline uint32_t z_vrfy_svc_cycles(void)
{
	return z_impl_svc_cycles();
}
#include <zephyr/syscalls/svc_cycles_mrsh.c>

static inline int z_vrfy_svc_die_temp_milli(void)
{
	return z_impl_svc_die_temp_milli();
}
#include <zephyr/syscalls/svc_die_temp_milli_mrsh.c>

static inline int z_vrfy_svc_kv_set(const char *key, uint32_t value)
{
	char buf[KV_KEY_MAX];

	if (k_usermode_string_copy(buf, (char *)key, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_kv_set(buf, value);
}
#include <zephyr/syscalls/svc_kv_set_mrsh.c>

static inline int z_vrfy_svc_kv_get(const char *key, uint32_t *out)
{
	char buf[KV_KEY_MAX];

	if (k_usermode_string_copy(buf, (char *)key, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_kv_get(buf, out);
}
#include <zephyr/syscalls/svc_kv_get_mrsh.c>

static inline int z_vrfy_svc_flash_read(uint32_t off, uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_flash_read(off, buf, len);
}
#include <zephyr/syscalls/svc_flash_read_mrsh.c>

static inline int z_vrfy_svc_flash_write(uint32_t off, const uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_flash_write(off, buf, len);
}
#include <zephyr/syscalls/svc_flash_write_mrsh.c>

static inline int z_vrfy_svc_flash_erase(uint32_t off, size_t len)
{
	return z_impl_svc_flash_erase(off, len);
}
#include <zephyr/syscalls/svc_flash_erase_mrsh.c>

static inline int z_vrfy_svc_device_id(uint8_t *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_device_id(buf, len);
}
#include <zephyr/syscalls/svc_device_id_mrsh.c>

static inline int z_vrfy_svc_reset_cause(uint32_t *cause)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(cause, sizeof(*cause)));
	return z_impl_svc_reset_cause(cause);
}
#include <zephyr/syscalls/svc_reset_cause_mrsh.c>

static inline int z_vrfy_svc_timer_start(uint32_t ms)
{
	return z_impl_svc_timer_start(ms);
}
#include <zephyr/syscalls/svc_timer_start_mrsh.c>

static inline int z_vrfy_svc_timer_wait(uint32_t timeout_ms)
{
	return z_impl_svc_timer_wait(timeout_ms);
}
#include <zephyr/syscalls/svc_timer_wait_mrsh.c>

static inline int z_vrfy_svc_rtc_set(uint32_t epoch_sec)
{
	return z_impl_svc_rtc_set(epoch_sec);
}
#include <zephyr/syscalls/svc_rtc_set_mrsh.c>

static inline int z_vrfy_svc_rtc_now(uint32_t *epoch_sec)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(epoch_sec, sizeof(*epoch_sec)));
	return z_impl_svc_rtc_now(epoch_sec);
}
#include <zephyr/syscalls/svc_rtc_now_mrsh.c>

static inline int z_vrfy_svc_ipc_send(int dest_id, const void *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_ipc_send(dest_id, buf, len);
}
#include <zephyr/syscalls/svc_ipc_send_mrsh.c>

static inline int z_vrfy_svc_ipc_recv(void *buf, size_t len, uint32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, len));
	return z_impl_svc_ipc_recv(buf, len, timeout_ms);
}
#include <zephyr/syscalls/svc_ipc_recv_mrsh.c>

static inline int z_vrfy_svc_app_count(void)
{
	return z_impl_svc_app_count();
}
#include <zephyr/syscalls/svc_app_count_mrsh.c>

static inline int z_vrfy_svc_app_info(int id, struct svc_app_info *out)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));
	return z_impl_svc_app_info(id, out);
}
#include <zephyr/syscalls/svc_app_info_mrsh.c>
#endif
