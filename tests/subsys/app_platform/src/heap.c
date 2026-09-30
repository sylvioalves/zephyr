/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Per-application heap. */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_platform/app_heap.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#include "module_run.h"

#define HEAP_SZ  992U
#define POOL_SZ  CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE
#define HEAP_MAX CONFIG_APP_PLATFORM_APP_HEAP_MAX_BYTES
#define PATTERN  0xa5c3e1f7U
#define ROUNDS   200
#define THR_CAPS (APP_CAP_LOG | APP_CAP_THREAD)

/* Argument that makes the test module exercise its heap and return. */
#define HEAP_PROBE ((void *)0x4850U)

static const uint8_t mod_bytes[] = {
#include "test_mod.inc"
};
static const uint8_t mod_manifest[] = {
#include "test_mod.heap.manifest.inc"
};
static const uint8_t mod_sig[] = {
#include "test_mod.heap.sig.inc"
};
static const uint8_t mod_pubkey[] = {
#include "test_mod.pubkey.inc"
};

K_APPMEM_PARTITION_DEFINE(heap_part);

struct heap_report {
	int rc[4];
	uintptr_t base;
	size_t size;
	uint32_t found;
	int mode;
	bool done;
};

K_APP_BMEM(heap_part) static struct heap_report rep;

#ifdef CONFIG_USERSPACE
/* Domains the report partition was added to. */
static struct k_mem_domain *doms[CONFIG_APP_PLATFORM_MAX_APPS];

static bool has_heap_part(struct k_mem_domain *dom)
{
	for (int i = 0; i < CONFIG_MAX_DOMAIN_PARTITIONS; i++) {
		if (dom->partitions[i].start == heap_part.start &&
		    dom->partitions[i].size == heap_part.size) {
			return true;
		}
	}
	return false;
}
#endif

static int slot_of(const char *name)
{
	for (int i = 0; i < app_platform_count(); i++) {
		struct app_platform_status st;

		if (app_platform_status(i, &st) == 0 && st.name != NULL &&
		    strcmp(st.name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool wait_slot_free(const char *name, int ms)
{
	for (int i = 0; i < ms / 5; i++) {
		if (slot_of(name) < 0) {
			k_msleep(10);
			return true;
		}
		k_msleep(5);
	}
	return slot_of(name) < 0;
}

static bool wait_done(int ms)
{
	for (int i = 0; i < ms / 5 && !rep.done; i++) {
		k_msleep(5);
	}
	return rep.done;
}

static int launch(const char *name, app_entry_t entry, app_caps_t caps, size_t heap_size)
{
	const struct app_def def = {
		.name = name,
		.source = APP_BUILTIN,
		.entry = entry,
		.caps = caps,
		.heap_size = heap_size,
		.stopped = true,
	};
	int id = app_platform_launch(&def);

#ifdef CONFIG_USERSPACE
	if (id >= 0) {
		struct k_mem_domain *dom = app_platform_domain(id);

		if (!has_heap_part(dom)) {
			zassert_equal(k_mem_domain_add_partition(dom, &heap_part), 0);
		}
		doms[id] = dom;
	}
#endif
	if (id >= 0) {
		zassert_equal(app_platform_start(id), id, "%s did not start", name);
	}
	return id;
}

static size_t heap_bytes(int id)
{
	struct app_platform_status st;

	zassert_equal(app_platform_status(id, &st), 0);
	return st.heap_bytes;
}

static bool in_region(const void *p, size_t len, uintptr_t base, size_t size)
{
	return (uintptr_t)p >= base && (uintptr_t)p + len <= base + size;
}

static void basic_app(void *arg)
{
	uintptr_t base = 0U;
	size_t size = 0U;
	uint8_t *p;
	uint8_t *q;
	uint8_t *a;
	uint32_t *z;

	ARG_UNUSED(arg);
	rep.rc[0] = app_svc_heap_info(&base, &size);
	rep.base = base;
	rep.size = size;

	p = app_malloc(100);
	if (p == NULL || !in_region(p, 100, base, size)) {
		rep.rc[1] = 1;
		goto out;
	}
	(void)memset(p, 0x5a, 100);
	q = app_realloc(p, 300);
	if (q == NULL || !in_region(q, 300, base, size)) {
		rep.rc[1] = 2;
		goto out;
	}
	for (int i = 0; i < 100; i++) {
		if (q[i] != 0x5aU) {
			rep.rc[1] = 3;
			goto out;
		}
	}
	a = app_aligned_alloc(64, 40);
	if (a == NULL || ((uintptr_t)a % 64U) != 0U || !in_region(a, 40, base, size)) {
		rep.rc[1] = 4;
		goto out;
	}
	app_free(q);
	app_free(a);
	app_free(NULL);

	p = app_malloc(64);
	if (p == NULL) {
		rep.rc[1] = 5;
		goto out;
	}
	(void)memset(p, 0xff, 64);
	app_free(p);
	z = app_calloc(16, sizeof(uint32_t));
	if (z == NULL) {
		rep.rc[1] = 6;
		goto out;
	}
	for (int i = 0; i < 16; i++) {
		if (z[i] != 0U) {
			rep.rc[1] = 7;
			goto out;
		}
	}
	app_free(z);
	if (app_calloc(SIZE_MAX / 2U, 4U) != NULL) {
		rep.rc[1] = 8;
	}
out:
	rep.done = true;
}

ZTEST(memheap, test_alloc_free_calloc_realloc_aligned)
{
	int id = launch("hbasic", basic_app, APP_CAP_LOG, HEAP_SZ);
	size_t bytes;

	zassert_true(id >= 0, "launch rc %d", id);
	bytes = heap_bytes(id);
	zassert_true(bytes >= HEAP_SZ + sizeof(struct app_heap_region), "heap of %zu bytes", bytes);
	zassert_true(wait_slot_free("hbasic", 3000), "hbasic never ended");
	zassert_equal(rep.rc[0], 0, "heap info rc %d", rep.rc[0]);
	zassert_equal(rep.size, bytes, "region %zu, status %zu", rep.size, bytes);
	zassert_equal(rep.rc[1], 0, "allocator check %d failed", rep.rc[1]);
}

static void exhaust_app(void *arg)
{
	void *head = NULL;
	int n = 0;

	ARG_UNUSED(arg);
	for (;;) {
		void **b = app_malloc(32);

		if (b == NULL) {
			break;
		}
		*b = head;
		head = b;
		n++;
	}
	rep.rc[0] = n;
	while (head != NULL) {
		void *next = *(void **)head;

		app_free(head);
		head = next;
	}
	rep.rc[1] = (app_malloc(HEAP_SZ / 2U) != NULL) ? 0 : -ENOMEM;
	rep.done = true;
}

ZTEST(memheap, test_exhaustion_returns_null)
{
	int id = launch("hfull", exhaust_app, APP_CAP_LOG, HEAP_SZ);
	size_t bytes;

	zassert_true(id >= 0, "launch rc %d", id);
	bytes = heap_bytes(id);
	zassert_true(wait_slot_free("hfull", 3000), "hfull never ended");
	zassert_true(rep.rc[0] > 0, "no allocation succeeded");
	zassert_true((size_t)rep.rc[0] * 32U <= bytes, "%d blocks from %zu bytes", rep.rc[0],
		     bytes);
	zassert_equal(rep.rc[1], 0, "freed heap did not coalesce");
}

static void dirty_app(void *arg)
{
	uintptr_t base = 0U;
	size_t size = 0U;

	ARG_UNUSED(arg);
	if (app_svc_heap_info(&base, &size) != 0) {
		rep.rc[0] = -ENOENT;
	} else if (rep.mode == 0) {
		uint32_t *p = app_malloc(HEAP_SZ / 2U);

		for (size_t i = 0U; p != NULL && i < HEAP_SZ / 2U / sizeof(uint32_t); i++) {
			p[i] = PATTERN;
		}
		rep.rc[0] = (p != NULL) ? 0 : -ENOMEM;
	} else {
		const uint32_t *w = (const uint32_t *)base;

		rep.found = 0U;
		for (size_t i = 0U; i < size / sizeof(uint32_t); i++) {
			if (w[i] == PATTERN) {
				rep.found++;
			}
		}
		rep.rc[0] = 0;
	}
	rep.done = true;
	for (;;) {
		k_msleep(10);
	}
}

static void dirty_step(int id, int mode)
{
	rep.mode = mode;
	rep.done = false;
	rep.found = UINT32_MAX;
	zassert_equal(app_platform_stop(id), 0);
	k_msleep(20);
	zassert_true(app_platform_start(id) >= 0);
	zassert_true(wait_done(2000), "application did not run");
	zassert_equal(rep.rc[0], 0, "rc %d", rep.rc[0]);
}

ZTEST(memheap, test_region_zeroed_for_restart_and_next_app)
{
	int id = launch("hdirty", dirty_app, APP_CAP_LOG, HEAP_SZ);
	int id2;

	zassert_true(id >= 0, "launch rc %d", id);
	zassert_true(wait_done(2000));
	zassert_equal(rep.rc[0], 0, "fill rc %d", rep.rc[0]);

	dirty_step(id, 1);
	zassert_equal(rep.found, 0U, "a restart kept %u words", rep.found);

	dirty_step(id, 0);
	zassert_equal(app_platform_remove(id), 0);
	zassert_true(wait_slot_free("hdirty", 3000));

	rep.mode = 1;
	rep.done = false;
	rep.found = UINT32_MAX;
	id2 = launch("hscan", dirty_app, APP_CAP_LOG, HEAP_SZ);
	zassert_equal(id2, id, "not the same slot (%d, %d)", id2, id);
	zassert_true(wait_done(2000));
	zassert_equal(rep.found, 0U, "the next application saw %u words", rep.found);
	zassert_equal(app_platform_remove(id2), 0);
	zassert_true(wait_slot_free("hscan", 3000));
}

static void quit_app(void *arg)
{
	ARG_UNUSED(arg);
}

ZTEST(memheap, test_launches_do_not_leak_the_pool)
{
	for (int round = 0; round < 24; round++) {
		int id = launch("hleak", quit_app, APP_CAP_LOG, HEAP_SZ);

		zassert_true(id >= 0, "round %d launch rc %d", round, id);
		zassert_true(wait_slot_free("hleak", 3000), "round %d not reclaimed", round);
	}
}

ZTEST(memheap, test_heap_above_limits_refused)
{
	int id = launch("hbig", quit_app, APP_CAP_LOG, HEAP_MAX + 1U);

	zassert_equal(id, -E2BIG, "rc %d", id);
	zassert_true(slot_of("hbig") < 0, "a refused launch kept its slot");

	if (POOL_SZ <= HEAP_MAX) {
		id = launch("hbig", quit_app, APP_CAP_LOG, POOL_SZ);
		zassert_equal(id, -ENOMEM, "rc %d", id);
		zassert_true(slot_of("hbig") < 0, "a refused launch kept its slot");
	}

	id = launch("hbig", quit_app, APP_CAP_LOG, HEAP_SZ);
	zassert_true(id >= 0, "launch after refusals rc %d", id);
	zassert_true(wait_slot_free("hbig", 3000));
}

static void no_heap_app(void *arg)
{
	uintptr_t base;
	size_t size;

	ARG_UNUSED(arg);
	rep.rc[0] = app_svc_heap_info(&base, &size);
	rep.rc[1] = (app_malloc(8) == NULL) ? 0 : 1;
	rep.done = true;
}

ZTEST(memheap, test_no_heap_declared)
{
	uintptr_t base;
	size_t size;
	int id = launch("hnone", no_heap_app, APP_CAP_LOG, 0U);

	zassert_true(id >= 0, "launch rc %d", id);
	zassert_equal(heap_bytes(id), 0U);
	zassert_true(wait_slot_free("hnone", 3000));
	zassert_equal(rep.rc[0], -ENOENT, "heap info rc %d", rep.rc[0]);
	zassert_equal(rep.rc[1], 0, "allocated without a heap");
	zassert_equal(app_svc_heap_info(&base, &size), -EPERM);
}

static int churn(int tag)
{
	int bad = 0;

	for (int i = 0; i < ROUNDS; i++) {
		size_t len = 8U + (size_t)((i * 7 + tag * 13) % 57);
		uint8_t *p = app_malloc(len);

		if (p == NULL) {
			bad++;
			continue;
		}
		(void)memset(p, tag, len);
		k_yield();
		for (size_t j = 0U; j < len; j++) {
			if (p[j] != (uint8_t)tag) {
				bad++;
				break;
			}
		}
		app_free(p);
	}
	return bad;
}

static void churn_worker(void *arg)
{
	int tag = (int)(uintptr_t)arg;

	rep.rc[tag] = churn(tag);
}

static void churn_app(void *arg)
{
	int h1 = app_svc_thread_spawn(churn_worker, (void *)1, 0);
	int h2 = app_svc_thread_spawn(churn_worker, (void *)2, 1);

	ARG_UNUSED(arg);
	rep.rc[3] = churn(3);
	rep.rc[0] = (h1 > 0 && h2 > 0) ? 0 : -EAGAIN;
	if (h1 > 0) {
		(void)app_svc_thread_join(h1, -1);
	}
	if (h2 > 0) {
		(void)app_svc_thread_join(h2, -1);
	}
	rep.done = true;
}

ZTEST(memheap, test_threads_share_the_heap)
{
	if (CONFIG_APP_PLATFORM_APP_THREADS < 2) {
		ztest_test_skip();
	}
	for (int i = 1; i < 4; i++) {
		rep.rc[i] = -1;
	}
	zassert_true(launch("hthr", churn_app, THR_CAPS, HEAP_SZ) >= 0);
	zassert_true(wait_slot_free("hthr", 5000), "hthr never ended");
	zassert_true(rep.done);
	zassert_equal(rep.rc[0], 0, "spawn failed");
	for (int i = 1; i < 4; i++) {
		zassert_equal(rep.rc[i], 0, "thread %d saw %d bad blocks", i, rep.rc[i]);
	}
}

ZTEST(memheap, test_loaded_module_gets_its_signed_heap)
{
	struct app_manifest mf;
	struct llext *ext = NULL;
	void *entry = NULL;
	uint8_t *buf = k_malloc(sizeof(mod_bytes));
	int id;

	zassert_not_null(buf);
	memcpy(buf, mod_bytes, sizeof(mod_bytes));
	memcpy(&mf, mod_manifest, sizeof(mf));
	zassert_true(mf.heap_size > 0U, "the test module declares no heap");

	zassert_equal(app_loader_verify_load_manifest(&mf, sizeof(mf), mod_sig, sizeof(mod_sig),
						      buf, sizeof(mod_bytes), mod_pubkey,
						      sizeof(mod_pubkey), "app_main", &ext, &entry),
		      0);
	zassert_equal(app_loader_heap_size(ext), mf.heap_size);

#ifdef CONFIG_USERSPACE
	int need = 1;

	for (int i = 0; i < LLEXT_MEM_PARTITIONS; i++) {
		need += (ext->mem_parts[i].size != 0U) ? 1 : 0;
	}
#ifdef Z_LIBC_PARTITION_EXISTS
	need++;
#endif
	if (need > arch_mem_domain_max_partitions_get()) {
		app_loader_forget(ext);
		llext_unload(&ext);
		k_free(buf);
		ztest_test_skip();
	}
#endif

	module_watch(mf.name);
	id = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, HEAP_PROBE, APP_CAP_LOG);
	zassert_true(id >= 0, "launch rc %d", id);
	zassert_true(heap_bytes(id) >= mf.heap_size, "heap of %zu bytes", heap_bytes(id));
	zassert_equal(module_wait_end(id, 3000), 0, "the module's heap probe failed");
	k_free(buf);
}

static void heap_before(void *f)
{
	ARG_UNUSED(f);
	memset(&rep, 0, sizeof(rep));
}

/* Leave the slots' domains with the partitions the other suites expect. */
static void heap_after(void *f)
{
	ARG_UNUSED(f);
#ifdef CONFIG_USERSPACE
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		if (doms[i] != NULL && has_heap_part(doms[i])) {
			(void)k_mem_domain_remove_partition(doms[i], &heap_part);
		}
		doms[i] = NULL;
	}
#endif
}

ZTEST_SUITE(memheap, NULL, NULL, heap_before, heap_after, NULL);
