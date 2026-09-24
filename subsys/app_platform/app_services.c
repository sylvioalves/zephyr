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
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(app_log, CONFIG_APP_PLATFORM_LOG_LEVEL);

/* Per-app log output can be muted at runtime so it does not crowd the shell. */
static bool app_log_enabled = true;

void app_platform_log_enable(bool en)
{
	app_log_enabled = en;
}

bool app_platform_log_is_enabled(void)
{
	return app_log_enabled;
}
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

/*
 * Bound how long an application waits for a shared-resource lock. The protected
 * sections are short, so reaching this cap means the resource is genuinely
 * wedged; the service then returns -EBUSY instead of parking the caller for
 * ever, so a stuck or greedy application cannot block a well-behaved one.
 */
#define SVC_LOCK_TIMEOUT K_SECONDS(2)

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

#if defined(CONFIG_SENSOR) && DT_NODE_HAS_STATUS(DT_ALIAS(die_temp0), okay)
static const struct device *const svc_die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));
#endif

/*
 * Per-application mailbox: a fixed-size message queue, one per group. The
 * queues and their backing storage are arrays sized by the number of
 * application groups, so the messaging service scales with
 * CONFIG_APP_PLATFORM_MAX_APPS without per-group boilerplate.
 */
#define MAX_APPS  CONFIG_APP_PLATFORM_MAX_APPS
#define IPC_MSG_MAX 64
#define IPC_QUEUE_DEPTH 4

struct ipc_msg {
	uint16_t len;
	uint8_t  data[IPC_MSG_MAX];
};

static struct k_msgq mailbox[MAX_APPS];
static char mailbox_buf[MAX_APPS][IPC_QUEUE_DEPTH * sizeof(struct ipc_msg)]
	__aligned(4);

static int ipc_mailbox_init(void)
{
	for (int i = 0; i < MAX_APPS; i++) {
		k_msgq_init(&mailbox[i], mailbox_buf[i], sizeof(struct ipc_msg),
			    IPC_QUEUE_DEPTH);
	}
	return 0;
}
SYS_INIT(ipc_mailbox_init, PRE_KERNEL_1, 0);

/* Optional name and topic subscription per application, for addressed and
 * published messaging on top of the mailboxes.
 */
#define IPC_NAME_MAX  16
#define IPC_TOPIC_MAX 24

static char ipc_names[MAX_APPS][IPC_NAME_MAX];
static char ipc_topics[MAX_APPS][IPC_TOPIC_MAX];
static K_MUTEX_DEFINE(ipc_dir_lock);

void app_platform_clear_ipc(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	k_msgq_purge(&mailbox[id]);
	k_mutex_lock(&ipc_dir_lock, K_FOREVER);
	ipc_names[id][0] = '\0';
	ipc_topics[id][0] = '\0';
	k_mutex_unlock(&ipc_dir_lock);
}

/* Copy a message into one application's mailbox. */
static int mailbox_put(int dest, const void *buf, size_t len)
{
	struct ipc_msg msg;

	msg.len = (uint16_t)len;
	memcpy(msg.data, buf, len);
	return k_msgq_put(&mailbox[dest], &msg, K_NO_WAIT) == 0 ? 0 : -EAGAIN;
}

/* True when the calling application holds every bit in @mask. */
static inline bool app_has_cap(app_caps_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

void z_impl_svc_log(const char *msg)
{
	if (!app_has_cap(APP_CAP_LOG)) {
		return;
	}
	if (!app_log_enabled) {
		return;
	}
	/* Route through the logging subsystem so output cooperates with the
	 * shell (rendered above the prompt) instead of racing it on the console.
	 */
	LOG_INF("[app %d] %s", app_platform_id_current(), msg);
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
#if defined(CONFIG_SENSOR) && DT_NODE_HAS_STATUS(DT_ALIAS(die_temp0), okay)
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

	if (k_mutex_lock(&kv_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
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

	if (k_mutex_lock(&kv_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
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

/* Drop every key an application owned. The store is keyed by group id and a
 * slot is handed to a new application once its previous tenant is reclaimed,
 * so without this the newcomer would inherit the old keys as its own.
 */
static void kv_release(int id)
{
	if (k_mutex_lock(&kv_lock, SVC_LOCK_TIMEOUT) != 0) {
		return;
	}
	for (int i = 0; i < KV_SLOTS; i++) {
		if (kv_store[i].used && kv_store[i].owner == id) {
			(void)memset(&kv_store[i], 0, sizeof(kv_store[i]));
		}
	}
	k_mutex_unlock(&kv_lock);
}

static int kv_service_init(void)
{
	return app_platform_on_reclaim(kv_release);
}
SYS_INIT(kv_service_init, APPLICATION, 99);

int z_impl_svc_kv_delete(const char *key)
{
	if (!app_has_cap(APP_CAP_KV)) {
		return -EPERM;
	}

	int owner = app_platform_id_current();
	int rc = -ENOENT;

	if (k_mutex_lock(&kv_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	for (int i = 0; i < KV_SLOTS; i++) {
		if (kv_store[i].used && kv_store[i].owner == owner &&
		    strcmp(kv_store[i].key, key) == 0) {
			/* Clear the key too, so the freed slot keeps no trace
			 * of what the previous tenant stored.
			 */
			(void)memset(&kv_store[i], 0, sizeof(kv_store[i]));
			rc = 0;
			break;
		}
	}
	k_mutex_unlock(&kv_lock);
	return rc;
}

/* Per-application window into the storage partition (len 0 = no grant). The
 * partition also holds the registry and the signed modules, so an application
 * reaches only the range the platform assigned to it.
 */
static struct {
	uint32_t base;
	uint32_t len;
} flash_grant[MAX_APPS];

/* Drop the flash window of a slot being handed to a new application. */
void app_platform_clear_flash_grants(int id)
{
	if (id >= 0 && id < MAX_APPS) {
		flash_grant[id].base = 0U;
		flash_grant[id].len = 0U;
	}
}

void app_platform_grant_flash(int id, uint32_t base, uint32_t len)
{
	if (id >= 0 && id < MAX_APPS) {
		flash_grant[id].base = base;
		flash_grant[id].len = len;
	}
}

#if defined(CONFIG_FLASH_MAP) && DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
/* Translate an application-relative range to an absolute partition offset.
 * Returns 0 and stores the offset when the whole range lies inside the
 * application's window. Only the flash services call it, so it follows their
 * guard rather than being built for a target that has no storage partition.
 */
static int flash_window(uint32_t off, size_t len, uint32_t size, uint32_t *abs)
{
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	uint32_t base = flash_grant[id].base;
	uint32_t win = flash_grant[id].len;

	if (win == 0U) {
		return -EACCES;
	}
	if (off > win || len > win - off) {
		return -EACCES;
	}
	if (base > size || win > size - base) {
		return -EACCES;
	}
	*abs = base + off;
	return 0;
}
#endif /* CONFIG_FLASH_MAP && storage_partition */

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

	if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
		return -EIO;
	}

	uint32_t abs_off;
	int rc = flash_window(off, len, fa->fa_size, &abs_off);

	if (rc == 0) {
		if (k_mutex_lock(&flash_lock, SVC_LOCK_TIMEOUT) != 0) {
			flash_area_close(fa);
			return -EBUSY;
		}
		rc = flash_area_read(fa, abs_off, buf, len);
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

	if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
		return -EIO;
	}

	uint32_t abs_off;
	int rc = flash_window(off, len, fa->fa_size, &abs_off);

	if (rc == 0) {
		if (k_mutex_lock(&flash_lock, SVC_LOCK_TIMEOUT) != 0) {
			flash_area_close(fa);
			return -EBUSY;
		}
		rc = flash_area_write(fa, abs_off, buf, len);
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

	if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
		return -EIO;
	}

	/* Erase spans whole sectors, but never past the application's window. */
	uint32_t abs_off;
	int rc = flash_window(off, len, fa->fa_size, &abs_off);

	if (rc == 0) {
		if (k_mutex_lock(&flash_lock, SVC_LOCK_TIMEOUT) != 0) {
			flash_area_close(fa);
			return -EBUSY;
		}
		rc = flash_area_erase(fa, abs_off, len);
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

/* Timer slots: each application owns APP_TIMER_SLOTS independent one-shot or
 * periodic timers, claimed and released through the svc_timer_* calls below.
 * Slots are a fixed array rather than allocations so a faulting application
 * cannot exhaust a heap, and the reclaim hook returns them all at once.
 */
struct app_timer_slot {
	struct k_timer timer;
	struct k_sem   sem;
	int64_t        armed_at;
	bool           used;
};

static struct app_timer_slot slots[MAX_APPS][APP_TIMER_SLOTS];
static struct k_spinlock slot_lock;

static void slot_expiry(struct k_timer *t)
{
	struct app_timer_slot *s = CONTAINER_OF(t, struct app_timer_slot, timer);

	k_sem_give(&s->sem);
}

static struct app_timer_slot *slot_of(int id, int slot)
{
	if (slot < 0 || slot >= APP_TIMER_SLOTS) {
		return NULL;
	}
	if (!slots[id][slot].used) {
		return NULL;
	}
	return &slots[id][slot];
}

/* Resolve the caller and its slot in one step, since every slot call needs
 * the capability, a valid group id and a claimed slot.
 */
static int slot_lookup(int slot, struct app_timer_slot **out)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}

	struct app_timer_slot *s = slot_of(id, slot);

	if (s == NULL) {
		return -EINVAL;
	}
	*out = s;
	return 0;
}

int z_impl_svc_timer_create(void)
{
	if (!app_has_cap(APP_CAP_TIME)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&slot_lock);

	for (int i = 0; i < APP_TIMER_SLOTS; i++) {
		if (!slots[id][i].used) {
			slots[id][i].used = true;
			k_spin_unlock(&slot_lock, key);
			k_timer_init(&slots[id][i].timer, slot_expiry, NULL);
			k_sem_init(&slots[id][i].sem, 0, 1);
			slots[id][i].armed_at = 0;
			return i;
		}
	}
	k_spin_unlock(&slot_lock, key);
	return -ENOSPC;
}

int z_impl_svc_timer_arm(int slot, uint32_t ms, int periodic)
{
	struct app_timer_slot *s;
	int err = slot_lookup(slot, &s);

	if (err != 0) {
		return err;
	}

	if (ms == 0U) {
		k_timer_stop(&s->timer);
		s->armed_at = 0;
		return 0;
	}

	k_sem_reset(&s->sem);
	s->armed_at = k_uptime_get();
	k_timer_start(&s->timer, K_MSEC(ms),
		      (periodic != 0) ? K_MSEC(ms) : K_NO_WAIT);
	return 0;
}

int z_impl_svc_timer_slot_wait(int slot, int32_t timeout_ms)
{
	struct app_timer_slot *s;
	int err = slot_lookup(slot, &s);

	if (err != 0) {
		return err;
	}

	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	return (k_sem_take(&s->sem, t) == 0) ? 0 : -EAGAIN;
}

int z_impl_svc_timer_stop(int slot)
{
	struct app_timer_slot *s;
	int err = slot_lookup(slot, &s);

	if (err != 0) {
		return err;
	}
	k_timer_stop(&s->timer);
	s->armed_at = 0;
	return 0;
}

int z_impl_svc_timer_elapsed(int slot)
{
	struct app_timer_slot *s;
	int err = slot_lookup(slot, &s);

	if (err != 0) {
		return err;
	}

	if (s->armed_at == 0) {
		return 0;
	}
	return (int)(k_uptime_get() - s->armed_at);
}

int z_impl_svc_timer_free(int slot)
{
	struct app_timer_slot *s;
	int err = slot_lookup(slot, &s);

	if (err != 0) {
		return err;
	}
	k_timer_stop(&s->timer);
	s->armed_at = 0;

	k_spinlock_key_t key = k_spin_lock(&slot_lock);

	s->used = false;
	k_spin_unlock(&slot_lock, key);
	return 0;
}

/* Return every slot the reclaimed application held. */
static void timer_slots_release(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}

	for (int i = 0; i < APP_TIMER_SLOTS; i++) {
		if (slots[id][i].used) {
			k_timer_stop(&slots[id][i].timer);
			slots[id][i].armed_at = 0;

			k_spinlock_key_t key = k_spin_lock(&slot_lock);

			slots[id][i].used = false;
			k_spin_unlock(&slot_lock, key);
		}
	}
}

static int timer_slots_init(void)
{
	return app_platform_on_reclaim(timer_slots_release);
}
SYS_INIT(timer_slots_init, APPLICATION, 99);

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
	if (k_mutex_lock(&rtc_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
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
	if (k_mutex_lock(&rtc_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
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
	return mailbox_put(dest_id, buf, len);
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

	if (k_msgq_get(&mailbox[id], &msg, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
	}

	size_t n = MIN(len, (size_t)msg.len);

	memcpy(buf, msg.data, n);
	return (int)n;
}

int z_impl_svc_ipc_register(const char *name)
{
	if (!app_has_cap(APP_CAP_IPC)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	if (k_mutex_lock(&ipc_dir_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	strncpy(ipc_names[id], name, IPC_NAME_MAX - 1);
	ipc_names[id][IPC_NAME_MAX - 1] = '\0';
	k_mutex_unlock(&ipc_dir_lock);
	return 0;
}

int z_impl_svc_ipc_send_to(const char *name, const void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_IPC)) {
		return -EPERM;
	}
	if (len > IPC_MSG_MAX) {
		return -EINVAL;
	}

	int dest = -1;

	if (k_mutex_lock(&ipc_dir_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	for (int i = 0; i < MAX_APPS; i++) {
		if (ipc_names[i][0] != '\0' && strcmp(ipc_names[i], name) == 0) {
			dest = i;
			break;
		}
	}
	k_mutex_unlock(&ipc_dir_lock);

	if (dest < 0) {
		return -ENOENT;
	}
	return mailbox_put(dest, buf, len);
}

int z_impl_svc_topic_subscribe(const char *topic)
{
	if (!app_has_cap(APP_CAP_IPC)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}
	if (k_mutex_lock(&ipc_dir_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	strncpy(ipc_topics[id], topic, IPC_TOPIC_MAX - 1);
	ipc_topics[id][IPC_TOPIC_MAX - 1] = '\0';
	k_mutex_unlock(&ipc_dir_lock);
	return 0;
}

int z_impl_svc_topic_publish(const char *topic, const void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_IPC)) {
		return -EPERM;
	}
	if (len > IPC_MSG_MAX) {
		return -EINVAL;
	}

	int delivered = 0;

	if (k_mutex_lock(&ipc_dir_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	for (int i = 0; i < MAX_APPS; i++) {
		if (ipc_topics[i][0] != '\0' && strcmp(ipc_topics[i], topic) == 0) {
			if (mailbox_put(i, buf, len) == 0) {
				delivered++;
			}
		}
	}
	k_mutex_unlock(&ipc_dir_lock);
	return delivered;
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

/* Stopping the caller would abort its thread inside this call, which never
 * returns, so the application would disappear instead of seeing an error.
 * Refuse it and let an application exit by returning from its entry point.
 */
int z_impl_svc_app_stop(int id)
{
	if (!app_has_cap(APP_CAP_CONTROL)) {
		return -EPERM;
	}
	if (id == app_platform_id_current()) {
		return -EINVAL;
	}
	return app_platform_stop(id);
}

int z_impl_svc_app_start(int id)
{
	if (!app_has_cap(APP_CAP_CONTROL)) {
		return -EPERM;
	}
	if (id == app_platform_id_current()) {
		return -EINVAL;
	}
#ifdef CONFIG_LLEXT
	/* app_platform_start() reports success as the group id it started, so
	 * normalise it: the service contract is 0 on success.
	 */
	int rc = app_platform_start(id);

	return rc < 0 ? rc : 0;
#else
	return -ENOTSUP;
#endif
}

int z_impl_svc_app_restart(int id)
{
	if (!app_has_cap(APP_CAP_CONTROL)) {
		return -EPERM;
	}
	if (id == app_platform_id_current()) {
		return -EINVAL;
	}
#ifdef CONFIG_LLEXT
	/* An application that is not running reports -EINVAL here, which is
	 * not an error for a restart: the caller asked for it to end up
	 * running, and starting a stopped one is the same operation. A busy
	 * platform still fails, since the stop did not take effect.
	 */
	int rc = app_platform_stop(id);

	if (rc != 0 && rc != -EINVAL) {
		return rc;
	}
	rc = app_platform_start(id);
	return rc < 0 ? rc : 0;
#else
	return -ENOTSUP;
#endif
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

static inline int z_vrfy_svc_kv_delete(const char *key)
{
	char buf[KV_KEY_MAX];

	if (k_usermode_string_copy(buf, (char *)key, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_kv_delete(buf);
}
#include <zephyr/syscalls/svc_kv_delete_mrsh.c>

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

static inline int z_vrfy_svc_timer_create(void)
{
	return z_impl_svc_timer_create();
}
#include <zephyr/syscalls/svc_timer_create_mrsh.c>

static inline int z_vrfy_svc_timer_arm(int slot, uint32_t ms, int periodic)
{
	return z_impl_svc_timer_arm(slot, ms, periodic);
}
#include <zephyr/syscalls/svc_timer_arm_mrsh.c>

static inline int z_vrfy_svc_timer_slot_wait(int slot, int32_t timeout_ms)
{
	return z_impl_svc_timer_slot_wait(slot, timeout_ms);
}
#include <zephyr/syscalls/svc_timer_slot_wait_mrsh.c>

static inline int z_vrfy_svc_timer_stop(int slot)
{
	return z_impl_svc_timer_stop(slot);
}
#include <zephyr/syscalls/svc_timer_stop_mrsh.c>

static inline int z_vrfy_svc_timer_elapsed(int slot)
{
	return z_impl_svc_timer_elapsed(slot);
}
#include <zephyr/syscalls/svc_timer_elapsed_mrsh.c>

static inline int z_vrfy_svc_timer_free(int slot)
{
	return z_impl_svc_timer_free(slot);
}
#include <zephyr/syscalls/svc_timer_free_mrsh.c>

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

static inline int z_vrfy_svc_ipc_register(const char *name)
{
	char buf[IPC_NAME_MAX];

	if (k_usermode_string_copy(buf, (char *)name, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_ipc_register(buf);
}
#include <zephyr/syscalls/svc_ipc_register_mrsh.c>

static inline int z_vrfy_svc_ipc_send_to(const char *name, const void *buf,
					 size_t len)
{
	char nbuf[IPC_NAME_MAX];

	if (k_usermode_string_copy(nbuf, (char *)name, sizeof(nbuf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_ipc_send_to(nbuf, buf, len);
}
#include <zephyr/syscalls/svc_ipc_send_to_mrsh.c>

static inline int z_vrfy_svc_topic_subscribe(const char *topic)
{
	char buf[IPC_TOPIC_MAX];

	if (k_usermode_string_copy(buf, (char *)topic, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_topic_subscribe(buf);
}
#include <zephyr/syscalls/svc_topic_subscribe_mrsh.c>

static inline int z_vrfy_svc_topic_publish(const char *topic, const void *buf,
					   size_t len)
{
	char tbuf[IPC_TOPIC_MAX];

	if (k_usermode_string_copy(tbuf, (char *)topic, sizeof(tbuf)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_topic_publish(tbuf, buf, len);
}
#include <zephyr/syscalls/svc_topic_publish_mrsh.c>

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

static inline int z_vrfy_svc_app_stop(int id)
{
	return z_impl_svc_app_stop(id);
}
#include <zephyr/syscalls/svc_app_stop_mrsh.c>

static inline int z_vrfy_svc_app_start(int id)
{
	return z_impl_svc_app_start(id);
}
#include <zephyr/syscalls/svc_app_start_mrsh.c>

static inline int z_vrfy_svc_app_restart(int id)
{
	return z_impl_svc_app_restart(id);
}
#include <zephyr/syscalls/svc_app_restart_mrsh.c>
#endif
