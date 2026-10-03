/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Bulk transfer through a ring the application owns. The platform allocates a
 * buffer, maps it into the calling application's domain and keeps a supervisor
 * pointer to it, so an application moves a batch of bytes and crosses the
 * syscall boundary once for the batch instead of once per transfer.
 *
 * The ring's head and tail live in application memory, which means the
 * application can rewrite them at any moment, including while the platform is
 * acting on them. Every access therefore re-reads the indices, bounds-checks
 * them against the buffer and works from the checked copy: a corrupt or
 * malicious ring costs the application its own data and nothing else.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <errno.h>
#include "app_grants.h"

/* The mapped region becomes a memory partition, and on an MPU or PMP a
 * partition must be a power of two and aligned to its own size.
 */
#define RING_MIN_BYTES 256U
#define RING_MAX_BYTES CONFIG_APP_PLATFORM_RING_MAX_BYTES

#if DT_HAS_CHOSEN(app_uart)
static const struct device *const ring_uart = DEVICE_DT_GET(DT_CHOSEN(app_uart));
#endif

struct ring_region {
	uint8_t                *base;  /* supervisor view of the buffer */
	size_t                  size;  /* whole mapping, header included */
	size_t                  cap;   /* usable data bytes */
#ifdef CONFIG_USERSPACE
	struct k_mem_partition  part;
#endif
	bool                    used;
};

static struct ring_region rings[MAX_APPS];
static SVC_LOCK_DEFINE(ring_lock);

static size_t round_pow2(size_t n)
{
	size_t p = RING_MIN_BYTES;

	while (p < n) {
		p <<= 1;
	}
	return p;
}

#if DT_HAS_CHOSEN(app_uart)
/* Read the application's indices once and check them before use. A value out
 * of range means the ring is corrupt, which is reported rather than clamped:
 * clamping would silently hand back the wrong bytes.
 */
static int ring_indices(const struct ring_region *r, uint32_t *head,
			uint32_t *tail)
{
	const struct app_ring_hdr *h = (const struct app_ring_hdr *)r->base;
	uint32_t hd = h->head;
	uint32_t tl = h->tail;

	if (hd >= r->cap || tl >= r->cap) {
		return -EBADMSG;
	}
	*head = hd;
	*tail = tl;
	return 0;
}
#endif

static struct ring_region *ring_of_current(int *out_id)
{
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_APPS || !rings[id].used) {
		return NULL;
	}
	if (out_id != NULL) {
		*out_id = id;
	}
	return &rings[id];
}

static void ring_release(int id)
{
	int rc;

	if (id < 0 || id >= MAX_APPS) {
		return;
	}
	rc = app_service_lock_reclaim(&ring_lock, SVC_LOCK_TIMEOUT);
	if (rc < 0) {
		return;
	}
	if (rings[id].used) {
#ifdef CONFIG_USERSPACE
		struct k_mem_domain *dom = app_platform_domain(id);

		if (dom != NULL) {
			(void)k_mem_domain_remove_partition(dom,
							    &rings[id].part);
		}
#endif
		if (rc == 0) {
			k_free(rings[id].base);
		}
		rings[id] = (struct ring_region){0};
	}
	if (rc == 0) {
		app_service_unlock(&ring_lock);
	}
}

int z_impl_app_svc_ring_bind(size_t bytes, uintptr_t *out_addr, size_t *out_cap)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}

	int id = app_platform_id_current();
	struct k_mem_domain *dom = app_platform_domain(id);

	if (id < 0 || id >= MAX_APPS || (IS_ENABLED(CONFIG_USERSPACE) && dom == NULL)) {
		return -EINVAL;
	}
	if (bytes == 0U || bytes > RING_MAX_BYTES) {
		return -EINVAL;
	}

	if (app_service_lock(&ring_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	if (rings[id].used) {
		app_service_unlock(&ring_lock);
		return -EEXIST;
	}

	size_t want = round_pow2(bytes + sizeof(struct app_ring_hdr));

	if (want > RING_MAX_BYTES) {
		app_service_unlock(&ring_lock);
		return -EINVAL;
	}

	uint8_t *base = k_aligned_alloc(want, want);

	if (base == NULL) {
		app_service_unlock(&ring_lock);
		return -ENOMEM;
	}
	(void)memset(base, 0, want);

	rings[id].base = base;
	rings[id].size = want;
	rings[id].cap = want - sizeof(struct app_ring_hdr);
#ifdef CONFIG_USERSPACE
	rings[id].part = (struct k_mem_partition){
		.start = (uintptr_t)base,
		.size = want,
		.attr = K_MEM_PARTITION_P_RW_U_RW,
	};

	int rc = k_mem_domain_add_partition(dom, &rings[id].part);

	if (rc != 0) {
		k_free(base);
		rings[id] = (struct ring_region){0};
		app_service_unlock(&ring_lock);
		return rc;
	}
#endif
	rings[id].used = true;
	app_service_unlock(&ring_lock);

	*out_addr = (uintptr_t)base;
	*out_cap = rings[id].cap;
	return 0;
}

int z_impl_app_svc_ring_unbind(void)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}
	if (ring_of_current(NULL) == NULL) {
		return -EINVAL;
	}
	ring_release(app_platform_id_current());
	return 0;
}

int z_impl_app_svc_ring_flush(void)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}

	struct ring_region *r = ring_of_current(NULL);

	if (r == NULL) {
		return -EINVAL;
	}
#if DT_HAS_CHOSEN(app_uart)
	if (!device_is_ready(ring_uart)) {
		return -ENODEV;
	}

	uint32_t head, tail;
	int rc = ring_indices(r, &head, &tail);

	if (rc != 0) {
		return rc;
	}

	uint8_t *data = r->base + sizeof(struct app_ring_hdr);
	int sent = 0;

	/* Walk the snapshot taken above rather than re-reading the header, so
	 * an application rewriting it mid-flush cannot steer this loop out of
	 * the buffer.
	 */
	while (tail != head) {
		uart_poll_out(ring_uart, data[tail]);
		tail = (tail + 1U) % r->cap;
		sent++;
	}

	/* Publish how far the platform got. The application owns head and is
	 * free to have moved it on; only tail is the platform's to report.
	 */
	((struct app_ring_hdr *)r->base)->tail = tail;
	return sent;
#else
	return -ENODEV;
#endif
}

int z_impl_app_svc_ring_fill(void)
{
	if (!app_has_cap(APP_CAP_UART)) {
		return -EPERM;
	}

	struct ring_region *r = ring_of_current(NULL);

	if (r == NULL) {
		return -EINVAL;
	}
#if DT_HAS_CHOSEN(app_uart)
	if (!device_is_ready(ring_uart)) {
		return -ENODEV;
	}

	uint32_t head, tail;
	int rc = ring_indices(r, &head, &tail);

	if (rc != 0) {
		return rc;
	}

	uint8_t *data = r->base + sizeof(struct app_ring_hdr);
	int got = 0;
	uint8_t c;

	/* Stop one short of the tail so a full ring stays distinguishable from
	 * an empty one, and never write past the checked capacity.
	 */
	while (((head + 1U) % r->cap) != tail &&
	       app_platform_uart_getc(&c) == 0) {
		data[head] = c;
		head = (head + 1U) % r->cap;
		got++;
	}

	((struct app_ring_hdr *)r->base)->head = head;
	return got;
#else
	return -ENODEV;
#endif
}

static int ring_service_init(void)
{
	return app_platform_on_reclaim(ring_release);
}
SYS_INIT(ring_service_init, APPLICATION, 99);

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_ring_bind(size_t bytes, uintptr_t *out_addr,
					   size_t *out_cap)
{
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out_addr, sizeof(*out_addr)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out_cap, sizeof(*out_cap)));
	return z_impl_app_svc_ring_bind(bytes, out_addr, out_cap);
}
#include <zephyr/syscalls/app_svc_ring_bind_mrsh.c>

static inline int z_vrfy_app_svc_ring_unbind(void)
{
	return z_impl_app_svc_ring_unbind();
}
#include <zephyr/syscalls/app_svc_ring_unbind_mrsh.c>

static inline int z_vrfy_app_svc_ring_flush(void)
{
	return z_impl_app_svc_ring_flush();
}
#include <zephyr/syscalls/app_svc_ring_flush_mrsh.c>

static inline int z_vrfy_app_svc_ring_fill(void)
{
	return z_impl_app_svc_ring_fill();
}
#include <zephyr/syscalls/app_svc_ring_fill_mrsh.c>
#endif
