/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Per-application event delivery. Input sources post events into a queue owned
 * by one application instead of a single shared semaphore, so one application
 * waiting no longer consumes another's events and one wait covers every source
 * an application is subscribed to. That is what lets an application dispatch
 * several sources from one loop, which a per-source blocking call cannot do.
 *
 * Producers run in interrupt context, so posting never blocks: when a queue is
 * full the oldest event is dropped and the sequence number skips, which is how
 * a reader learns it fell behind.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_caps.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/internal/syscall_handler.h>
#include <errno.h>
#include <string.h>

#include "app_grants.h"

#define QUEUE_LEN CONFIG_APP_PLATFORM_EVENT_QUEUE_LEN

struct app_queue {
	struct app_event buf[QUEUE_LEN];
	uint8_t          head;
	uint8_t          count;
	uint32_t         seq;
	struct k_sem     sem;
	bool             ready;
};

static struct app_queue queues[MAX_APPS];
static struct k_spinlock ev_lock;

/* Semaphores are initialized once at boot: k_sem_init() must not run with a
 * spinlock held, and a producer in an ISR must never find a half-built queue.
 */
static int events_init(void)
{
	for (int i = 0; i < MAX_APPS; i++) {
		k_sem_init(&queues[i].sem, 0, QUEUE_LEN);
		queues[i].ready = true;
	}
	return 0;
}
SYS_INIT(events_init, PRE_KERNEL_2, 0);

/* Post an event to one application. Safe from an ISR: the spinlock is held for
 * a copy and a couple of counter updates, and a full queue drops the oldest
 * event rather than waiting for the application to catch up.
 */
void app_events_post(int id, const struct app_event *evt)
{
	if (id < 0 || id >= MAX_APPS || evt == NULL) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&ev_lock);
	struct app_queue *q = &queues[id];
	bool dropped = false;

	if (!q->ready) {
		k_spin_unlock(&ev_lock, key);
		return;
	}
	if (q->count == QUEUE_LEN) {
		q->head = (uint8_t)((q->head + 1U) % QUEUE_LEN);
		q->count--;
		dropped = true;
	}

	uint8_t tail = (uint8_t)((q->head + q->count) % QUEUE_LEN);

	q->buf[tail] = *evt;
	q->buf[tail].seq = q->seq++;
	q->count++;
	k_spin_unlock(&ev_lock, key);

	/* The semaphore only wakes the reader; the queue holds the events, so a
	 * drop must not leave a count the queue cannot satisfy.
	 */
	if (!dropped) {
		k_sem_give(&q->sem);
	}
}

/* Post to every application subscribed through @pred. */
void app_events_broadcast(const struct app_event *evt,
			  bool (*pred)(int id))
{
	for (int id = 0; id < MAX_APPS; id++) {
		if (queues[id].ready && (pred == NULL || pred(id))) {
			app_events_post(id, evt);
		}
	}
}

int z_impl_svc_event_wait(struct app_event *evt, int32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_INPUT)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS) {
		return -EINVAL;
	}

	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	if (k_sem_take(&queues[id].sem, t) != 0) {
		return -EAGAIN;
	}

	k_spinlock_key_t key = k_spin_lock(&ev_lock);
	struct app_queue *q = &queues[id];
	struct app_event got;

	if (q->count == 0U) {
		k_spin_unlock(&ev_lock, key);
		return -EAGAIN;
	}
	got = q->buf[q->head];
	q->head = (uint8_t)((q->head + 1U) % QUEUE_LEN);
	q->count--;
	k_spin_unlock(&ev_lock, key);
	*evt = got;
	return 0;
}

/* Drop a slot's pending events when it is handed to a new application. */
void app_events_clear(int id)
{
	if (id < 0 || id >= MAX_APPS) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&ev_lock);
	struct app_queue *q = &queues[id];

	q->head = 0U;
	q->count = 0U;
	q->seq = 0U;
	k_spin_unlock(&ev_lock, key);
	if (q->ready) {
		k_sem_reset(&q->sem);
	}
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_event_wait(struct app_event *evt,
					int32_t timeout_ms)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(evt, sizeof(*evt)));
	return z_impl_svc_event_wait(evt, timeout_ms);
}
#include <zephyr/syscalls/svc_event_wait_mrsh.c>
#endif
