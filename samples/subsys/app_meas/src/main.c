/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Lifecycle timing and footprint measurements. */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_heap.h>
#include <string.h>

uint32_t app_meas_stamp[16];
#include <zephyr/multi_heap/shared_multi_heap.h>
extern struct k_heap _system_heap;
extern struct k_heap llext_heap;


static const uint8_t meas0_bytes[] = {
#include "meas0.inc"
};
static const uint8_t meas0_mf[] = {
#include "meas0.manifest.inc"
};
static const uint8_t meas0_sig[] = {
#include "meas0.sig.inc"
};
static const uint8_t meas0_sha[32] = {
#include "meas0.sha256.inc"
};
static const uint8_t meas4_bytes[] = {
#include "meas4.inc"
};
static const uint8_t meas4_mf[] = {
#include "meas4.manifest.inc"
};
static const uint8_t meas4_sig[] = {
#include "meas4.sig.inc"
};
static const uint8_t meas4_sha[32] = {
#include "meas4.sha256.inc"
};
static const uint8_t meas16_bytes[] = {
#include "meas16.inc"
};
static const uint8_t meas16_mf[] = {
#include "meas16.manifest.inc"
};
static const uint8_t meas16_sig[] = {
#include "meas16.sig.inc"
};
static const uint8_t meas16_sha[32] = {
#include "meas16.sha256.inc"
};
static const uint8_t meas64_bytes[] = {
#include "meas64.inc"
};
static const uint8_t meas64_mf[] = {
#include "meas64.manifest.inc"
};
static const uint8_t meas64_sig[] = {
#include "meas64.sig.inc"
};
static const uint8_t meas64_sha[32] = {
#include "meas64.sha256.inc"
};
static const uint8_t pubkey[] = {
#include "meas0.pubkey.inc"
};

struct mod {
	const char *name;
	const uint8_t *bytes;
	size_t len;
	const uint8_t *mf;
	size_t mf_len;
	const uint8_t *sig;
	const uint8_t *sha;
};

static const struct mod mods[] = {
	{"meas0", meas0_bytes, sizeof(meas0_bytes), meas0_mf, sizeof(meas0_mf), meas0_sig,
	 meas0_sha},
	{"meas4", meas4_bytes, sizeof(meas4_bytes), meas4_mf, sizeof(meas4_mf), meas4_sig,
	 meas4_sha},
	{"meas16", meas16_bytes, sizeof(meas16_bytes), meas16_mf, sizeof(meas16_mf), meas16_sig,
	 meas16_sha},
	{"meas64", meas64_bytes, sizeof(meas64_bytes), meas64_mf, sizeof(meas64_mf), meas64_sig,
	 meas64_sha},
};


/* The most a loaded application may hold: a signed module holds what its manifest
 * requests within it, a module checked by hash all of it.
 */
#define APP_CAPS_CEILING (APP_CAP_LOG | APP_CAP_TIME)

static inline uint32_t cyc(void)
{
	return (uint32_t)csr_read(mcycle);
}

static void print_time(const char *tag, uint32_t c)
{
	printk("  %-34s %9u cyc %8u us\n", tag, c, c / 240U);
}

static void heap_stats(const char *tag, size_t *sys_alloc, size_t *ext_alloc)
{
	struct sys_memory_stats s1, s2;

	sys_heap_runtime_stats_get(&_system_heap.heap, &s1);
	sys_heap_runtime_stats_get(&llext_heap.heap, &s2);
	if (sys_alloc) {
		*sys_alloc = s1.allocated_bytes;
	}
	if (ext_alloc) {
		*ext_alloc = s2.allocated_bytes;
	}
	if (tag) {
		printk("  %-34s sys heap alloc %6zu  llext heap alloc %6zu\n", tag,
		       s1.allocated_bytes, s2.allocated_bytes);
	}
}

static bool wait_slot_free(int id, int ms)
{
	for (int i = 0; i < ms / 5; i++) {
		struct app_platform_status st;

		if (app_platform_status(id, &st) == -EAGAIN) {
			return true;
		}
		k_msleep(5);
	}
	return false;
}

static void phase_load(const struct mod *m)
{
	union app_manifest_buf mb;
	struct llext *ext = NULL;
	void *entry = NULL;
	size_t sys0, ext0, sys1, ext1, sys2, ext2;

	memcpy(mb.raw, m->mf, MIN(m->mf_len, sizeof(mb)));
	printk("== %s: module %zu B (manifest %zu B + sig %d B)\n", m->name,
	       m->len, m->mf_len, APP_MANIFEST_SIG_LEN);
	heap_stats(NULL, &sys0, &ext0);
	memset(app_meas_stamp, 0, sizeof(app_meas_stamp));

	uint32_t t0 = cyc();
	int rc = app_loader_verify_load_manifest(&mb.mf, m->mf_len, m->sig, APP_MANIFEST_SIG_LEN,
						 m->bytes, m->len, pubkey,
						 sizeof(pubkey), "app_main",
						 &ext, &entry);
	uint32_t t1 = cyc();

	if (rc != 0) {
		printk("  verify/load FAILED rc=%d\n", rc);
		return;
	}
	print_time("manifest sha256", app_meas_stamp[1] - app_meas_stamp[0]);
	print_time("ecdsa p256 verify", app_meas_stamp[2] - app_meas_stamp[1]);
	print_time("module sha256", app_meas_stamp[3] - app_meas_stamp[2]);
	print_time("verify total (sha+ecdsa+sha)", app_meas_stamp[3] - app_meas_stamp[0]);
	print_time("llext_load (parse/alloc/reloc)", app_meas_stamp[5] - app_meas_stamp[4]);
	print_time("finish (sym lookup, budget)", app_meas_stamp[6] - app_meas_stamp[5]);
	print_time("verify+load total (caller)", t1 - t0);
	printk("  alloc_size %zu B\n", ext->alloc_size);
	heap_stats("after load", &sys1, &ext1);

	t0 = cyc();
	int id = app_platform_launch_llext(m->name, ext, (app_entry_t)entry, NULL,
					   APP_CAPS_CEILING);
	t1 = cyc();

	if (id < 0) {
		printk("  launch FAILED %d\n", id);
		return;
	}
	print_time("launch: domain init + map", app_meas_stamp[8] - app_meas_stamp[7]);
	print_time("launch: thread create+start", app_meas_stamp[9] - app_meas_stamp[8]);
	print_time("launch total (caller)", t1 - t0);
	k_msleep(30);
	heap_stats("after launch (app running)", &sys2, &ext2);

	struct app_platform_status st;

	if (app_platform_status(id, &st) == 0) {
		printk("  stack %zu B, used %zu B, module_bytes %zu B\n",
		       st.stack_size, st.stack_used, st.module_bytes);
	}
	printk("  RAM delta: llext heap +%zu, sys heap +%zu (load) +%zu (launch)\n",
	       ext2 - ext0, sys1 - sys0, sys2 - sys1);

	t0 = cyc();
	app_platform_remove(id);
	bool freed = wait_slot_free(id, 2000);

	t1 = cyc();
	printk("  remove + slot free: %s, %u us\n", freed ? "ok" : "TIMEOUT",
	       (t1 - t0) / 240U);
	heap_stats("after remove", NULL, NULL);
}

/* ---- 3.3 fault to reclaim ---- */

K_APPMEM_PARTITION_DEFINE(shared_part);
K_APP_DMEM(shared_part) volatile uint32_t t_app_last;

static void crasher(void *arg)
{
	ARG_UNUSED(arg);
	volatile uint32_t *p = NULL;

	t_app_last = (uint32_t)csr_read(cycle);
	*p = 0xdead;
}

static void hanger(void *arg)
{
	ARG_UNUSED(arg);
	for (int i = 0; i < 3; i++) {
		app_svc_heartbeat();
		k_msleep(100);
	}
	t_app_last = (uint32_t)csr_read(cycle);
	for (;;) {
		/* Silent from now on: no heartbeat, no syscall. */
	}
}

static void phase_fault(void)
{
	const struct app_def def = {
		.name = "crasher", .source = APP_BUILTIN, .entry = crasher,
		.caps = APP_CAP_LOG, .stopped = true,
	};

	printk("== 3.3 crash: NULL write in a built-in user app\n");
	memset(app_meas_stamp, 0, sizeof(app_meas_stamp));
	t_app_last = 0;
	int id = app_platform_launch(&def);

	k_mem_domain_add_partition(app_platform_domain(id), &shared_part);
	(void)app_platform_start(id);
	bool freed = wait_slot_free(id, 2000);

	printk("  slot free: %s\n", freed ? "yes" : "TIMEOUT");
	print_time("app store -> fatal handler", app_meas_stamp[10] - t_app_last);
	print_time("fatal handler -> supervisor", app_meas_stamp[11] - app_meas_stamp[10]);
	print_time("supervisor -> slot free (reaper)", app_meas_stamp[12] - app_meas_stamp[11]);
	print_time("total fault -> slot free", app_meas_stamp[12] - t_app_last);
}

static void phase_hang(void)
{
	const struct app_def def = {
		.name = "hanger", .source = APP_BUILTIN, .entry = hanger,
		.caps = APP_CAP_LOG, .monitored = true, .stopped = true,
	};

	printk("== 3.3 hang: monitored app stops heartbeating (WDT %d ms)\n",
	       CONFIG_APP_PLATFORM_WDT_TIMEOUT_MS);
	memset(app_meas_stamp, 0, sizeof(app_meas_stamp));
	t_app_last = 0;
	int id = app_platform_launch(&def);

	k_mem_domain_add_partition(app_platform_domain(id), &shared_part);
	(void)app_platform_start(id);
	bool freed = wait_slot_free(id, 6000);

	printk("  slot free: %s\n", freed ? "yes" : "TIMEOUT");
	printk("  last heartbeat -> wdt callback   %8u us\n",
	       (app_meas_stamp[13] - t_app_last) / 240U);
	print_time("wdt callback -> supervisor", app_meas_stamp[11] - app_meas_stamp[13]);
	print_time("supervisor -> slot free (reaper)", app_meas_stamp[12] - app_meas_stamp[11]);
	printk("  total silence -> slot free       %8u us\n",
	       (app_meas_stamp[12] - t_app_last) / 240U);
}

/* ---- 4.6 kernel pointer to app_svc_log ---- */

static uint32_t kernel_secret = 0x5ec2e7;

static void kptr_app(void *arg)
{
	ARG_UNUSED(arg);
	app_svc_log((const char *)&kernel_secret);
	app_svc_log((const char *)0xdeadbeef);
	app_svc_log((const char *)0x40800000);
	t_app_last = 0xa11ceU;
}

static void phase_kptr(void)
{
	const struct app_def def = {
		.name = "kptr", .source = APP_BUILTIN, .entry = kptr_app,
		.caps = APP_CAP_LOG, .stopped = true,
	};

	printk("== 4.6 app_svc_log with kernel/wild pointers (NLEN validate %s)\n",
	       IS_ENABLED(CONFIG_RISCV_USER_STRING_NLEN_VALIDATE) ? "on" : "off");
	t_app_last = 0;
	int id = app_platform_launch(&def);

	k_mem_domain_add_partition(app_platform_domain(id), &shared_part);
	(void)app_platform_start(id);
	bool freed = wait_slot_free(id, 2000);

	printk("  app finished normally: %s, slot free: %s\n",
	       t_app_last == 0xa11ceU ? "yes (copies refused, app alive)" : "no (aborted)",
	       freed ? "yes" : "no");
}

/* Heap placement comparison: hash-verified load + launch, caller-timed. */
static void phase_heapcmp(const struct mod *m)
{
	struct llext *ext = NULL;
	void *entry = NULL;
	uint32_t t0 = cyc();
	int rc = app_loader_verify_load(m->name, m->bytes, m->len, m->sha, "app_main",
					&ext, &entry);
	uint32_t t1 = cyc();

	if (rc != 0) {
		printk("  %s: load FAILED %d\n", m->name, rc);
		return;
	}
	int id = app_platform_launch_llext(m->name, ext, (app_entry_t)entry, NULL,
					   APP_CAPS_CEILING);
	uint32_t t2 = cyc();

	k_msleep(20);
	printk("  %-7s %6zu B: sha verify+load %6u us, launch %4u us, alloc %zu B at %p\n",
	       m->name, m->len, (t1 - t0) / 240U, (t2 - t1) / 240U, ext->alloc_size,
	       ext->mem[LLEXT_MEM_TEXT]);
	app_platform_remove(id);
	wait_slot_free(id, 2000);
}

int main(void)
{
	printk("\n*** app_meas: lifecycle timing (C5 @240 MHz) ***\n");
	csr_write(mcounteren, 0x7);
	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
#ifdef CONFIG_LLEXT_HEAP_DYNAMIC
	void *heap = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 64, 256 * 1024);

	if (heap == NULL || llext_heap_init(heap, 256 * 1024) != 0) {
		printk("PSRAM llext heap failed\n");
		return 0;
	}
	printk("== llext heap: 256 KB in PSRAM at %p\n", heap);
#else
	printk("== llext heap: %d KB static in SRAM\n", CONFIG_LLEXT_HEAP_SIZE);
#endif
	app_platform_log_enable(false);
	for (int r = 0; r < 2; r++) {
		for (size_t i = 0; i < ARRAY_SIZE(mods); i++) {
			phase_heapcmp(&mods[i]);
		}
	}
#ifdef HEAPCMP_ONLY
	printk("*** app_meas heapcmp done ***\n");
	return 0;
#endif
	heap_stats("baseline", NULL, NULL);
	for (size_t i = 0; i < ARRAY_SIZE(mods); i++) {
		phase_load(&mods[i]);
	}
	phase_fault();
	phase_hang();
	phase_kptr();
	printk("*** app_meas done ***\n");
	return 0;
}
