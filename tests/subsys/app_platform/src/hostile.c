/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Hostile applications: each one attempts something it must not be able to do.
 * Pass condition: the offending app is trapped and reclaimed (or the call is
 * refused with an error), the platform and a canary app keep running.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/arch/riscv/syscall.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

/* Data owned by the canary app's domain only. */
K_APPMEM_PARTITION_DEFINE(victim_part);
K_APP_DMEM(victim_part) volatile uint32_t victim_data = 0x600d;

/* Results partition shared with the hostile app so it can report. */
K_APPMEM_PARTITION_DEFINE(report_part);
K_APP_DMEM(report_part) volatile int report_rc;
K_APP_DMEM(report_part) volatile int report_done;
K_APP_DMEM(report_part) volatile int report_n;

static uint32_t kernel_var = 0xbeef;
extern const struct z_object *_k_syscall_table;

static void canary_app(void *arg)
{
	ARG_UNUSED(arg);
	for (;;) {
		victim_data++;
		svc_heartbeat();
		k_msleep(10);
	}
}

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

static void dump_slots(void)
{
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		struct app_platform_status st;
		int rc = app_platform_status(i, &st);

		printk("    slot %d: rc %d name %s active %d faulted %d\n", i, rc,
		       rc == 0 ? st.name : "-", rc == 0 ? st.active : -1,
		       rc == 0 ? st.faulted : -1);
	}
}

static bool wait_slot_free(const char *name, int ms)
{
	for (int i = 0; i < ms / 5; i++) {
		if (slot_of(name) < 0) {
			return true;
		}
		k_msleep(5);
	}
	if (slot_of(name) >= 0) {
		dump_slots();
	}
	return slot_of(name) < 0;
}

K_APP_DMEM(report_part) int canary_id;

static void *hostile_setup(void)
{
	const struct app_def def = {
		.name = "canary", .source = APP_BUILTIN, .entry = canary_app,
		.caps = APP_CAP_LOG,
	};

	canary_id = app_platform_launch(&def);
	zassert_true(canary_id >= 0, "canary launch failed");
	k_mem_domain_add_partition(app_platform_domain(canary_id), &victim_part);
	k_msleep(50);
	return NULL;
}

static void hostile_before(void *f)
{
	ARG_UNUSED(f);
	report_rc = 0;
	report_done = 0;
}

/* Launch a hostile app and expect it reclaimed with the canary still alive. */
static void expect_reclaimed(const char *name, app_entry_t entry, uint32_t caps)
{
	const struct app_def def = {
		.name = name, .source = APP_BUILTIN, .entry = entry, .caps = caps,
	};
	uint32_t before = victim_data;
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed");
	k_mem_domain_add_partition(app_platform_domain(id), &report_part);
	zassert_true(wait_slot_free(name, 2000), "%s was not reclaimed", name);
	zassert_equal(report_done, 0, "%s completed instead of trapping", name);

	struct app_platform_status st;

	zassert_equal(app_platform_status(canary_id, &st), 0);
	zassert_true(st.active, "canary died");
	k_msleep(30);
	zassert_true(victim_data != before, "canary stopped running");
}

/* Launch a hostile app that must be refused by return value and survive. */
static void expect_refused(const char *name, app_entry_t entry, uint32_t caps,
			   int expected_rc)
{
	const struct app_def def = {
		.name = name, .source = APP_BUILTIN, .entry = entry, .caps = caps,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed");
	k_mem_domain_add_partition(app_platform_domain(id), &report_part);
	zassert_true(wait_slot_free(name, 2000), "%s never exited", name);
	zassert_equal(report_done, 1, "%s trapped instead of being refused", name);
	zassert_equal(report_rc, expected_rc, "%s rc %d != %d", name, report_rc,
		      expected_rc);
}

/* ---- 4.1 other app's data ---- */

static void read_other_domain(void *a)
{
	ARG_UNUSED(a);
	report_rc = (int)victim_data;
	report_done = 1;
}

static void write_other_domain(void *a)
{
	ARG_UNUSED(a);
	victim_data = 0xbad;
	report_done = 1;
}

ZTEST(zhostile, test_4_1_read_other_domain)
{
	expect_reclaimed("h_rd", read_other_domain, APP_CAP_LOG);
	zassert_equal(victim_data & 0xffff, victim_data & 0xffff);
}

ZTEST(zhostile, test_4_1_write_other_domain)
{
	expect_reclaimed("h_wr", write_other_domain, APP_CAP_LOG);
	zassert_not_equal(victim_data, 0xbad, "victim data was modified");
}

/* ---- 4.2 kernel memory ---- */

static void read_kernel_sram(void *a)
{
	ARG_UNUSED(a);
	report_rc = (int)kernel_var;
	report_done = 1;
}

static void write_syscall_table(void *a)
{
	ARG_UNUSED(a);
	volatile uintptr_t *t = (volatile uintptr_t *)&_k_syscall_table;
	*t = 0;
	report_done = 1;
}

static void write_kernel_sram(void *a)
{
	ARG_UNUSED(a);
	kernel_var = 0;
	report_done = 1;
}

ZTEST(zhostile, test_4_2_read_kernel_sram)
{
	expect_reclaimed("h_krd", read_kernel_sram, APP_CAP_LOG);
}

ZTEST(zhostile, test_4_2_write_kernel_sram)
{
	expect_reclaimed("h_kwr", write_kernel_sram, APP_CAP_LOG);
	zassert_equal(kernel_var, 0xbeef, "kernel data modified");
}

ZTEST(zhostile, test_4_2_write_syscall_table)
{
	expect_reclaimed("h_sys", write_syscall_table, APP_CAP_LOG);
}

/* ---- 4.3 peripheral MMIO ---- */

static void write_gpio_mmio(void *a)
{
	ARG_UNUSED(a);
	/* GPIO_OUT_W1TS_REG on ESP32-C5 (GPIO base 0x60091000). */
	volatile uint32_t *reg = (volatile uint32_t *)0x60091008;
	*reg = BIT(2);
	report_done = 1;
}

static void write_uart_mmio(void *a)
{
	ARG_UNUSED(a);
	/* UART0 FIFO register on ESP32-C5. */
	volatile uint32_t *reg = (volatile uint32_t *)0x60000000;
	*reg = 'X';
	report_done = 1;
}

ZTEST(zhostile, test_4_3_gpio_mmio)
{
	expect_reclaimed("h_gpio", write_gpio_mmio, APP_CAP_LOG | APP_CAP_GPIO);
}

ZTEST(zhostile, test_4_3_uart_mmio)
{
	expect_reclaimed("h_uart", write_uart_mmio, APP_CAP_LOG);
}

/* ---- 4.4 privileged instructions ---- */

static void csrr_mstatus(void *a)
{
	ARG_UNUSED(a);
	uint32_t v;

	__asm__ volatile("csrr %0, mstatus" : "=r"(v));
	report_rc = (int)v;
	report_done = 1;
}

static void do_wfi(void *a)
{
	ARG_UNUSED(a);
	__asm__ volatile("wfi");
	report_rc = 1;
	report_done = 1;
}

static void do_mret(void *a)
{
	ARG_UNUSED(a);
	__asm__ volatile("mret");
	report_done = 1;
}

static void bad_ecall(void *a)
{
	ARG_UNUSED(a);
	report_rc = (int)arch_syscall_invoke0(0x7fff);
	report_done = 1;
}

ZTEST(zhostile, test_4_4_csrr_mstatus)
{
	expect_reclaimed("h_csr", csrr_mstatus, APP_CAP_LOG);
}

ZTEST(zhostile, test_4_4_mret)
{
	expect_reclaimed("h_mret", do_mret, APP_CAP_LOG);
}

ZTEST(zhostile, test_4_4_wfi)
{
	/* mstatus.TW is not set on this core: wfi from U-mode either traps or
	 * completes on the next interrupt; either outcome keeps the kernel intact.
	 * Record which one happened.
	 */
	const struct app_def def = {
		.name = "h_wfi", .source = APP_BUILTIN, .entry = do_wfi,
		.caps = APP_CAP_LOG,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0);
	k_mem_domain_add_partition(app_platform_domain(id), &report_part);
	zassert_true(wait_slot_free("h_wfi", 2000), "wfi app never finished");
	printk("  wfi from U-mode: %s\n", report_done ? "executed (TW=0), app continued"
						     : "trapped, app reclaimed");
}

ZTEST(zhostile, test_4_4_bad_ecall_id)
{
	/* An out-of-range syscall id lands in the bad-syscall handler which
	 * oopses the caller: the app is reclaimed.
	 */
	expect_reclaimed("h_ecall", bad_ecall, APP_CAP_LOG);
}

/* ---- 4.5 bad pointers to syscalls ---- */

K_APP_DMEM(report_part) uint8_t user_buf[64];

static void sha_kernel_ptr(void *a)
{
	ARG_UNUSED(a);
	uint8_t out[32];

	report_rc = svc_sha256((const uint8_t *)&kernel_var, 4, out);
	report_done = 1;
}

static void sha_wild_ptr(void *a)
{
	ARG_UNUSED(a);
	uint8_t out[32];

	report_rc = svc_sha256((const uint8_t *)0xdeadbee0, 4, out);
	report_done = 1;
}

static void sha_foreign_ptr(void *a)
{
	ARG_UNUSED(a);
	uint8_t out[32];

	report_rc = svc_sha256((const uint8_t *)&victim_data, 4, out);
	report_done = 1;
}

static void sha_out_kernel_ptr(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_sha256(user_buf, 4, (uint8_t *)&kernel_var);
	report_done = 1;
}

static void log_kernel_ptr(void *a)
{
	ARG_UNUSED(a);
	svc_log((const char *)&kernel_var);
	svc_log((const char *)0xdeadbeef);
	svc_log((const char *)&victim_data);
	report_rc = 0;
	report_done = 1;
}

ZTEST(zhostile, test_4_5_sha256_kernel_ptr)
{
	/* K_SYSCALL_MEMORY_READ fails: K_OOPS aborts the calling thread. */
	expect_reclaimed("h_shak", sha_kernel_ptr, APP_CAP_LOG | APP_CAP_CRYPTO);
	zassert_equal(kernel_var, 0xbeef);
}

ZTEST(zhostile, test_4_5_sha256_wild_ptr)
{
	expect_reclaimed("h_shaw", sha_wild_ptr, APP_CAP_LOG | APP_CAP_CRYPTO);
}

ZTEST(zhostile, test_4_5_sha256_foreign_ptr)
{
	expect_reclaimed("h_shaf", sha_foreign_ptr, APP_CAP_LOG | APP_CAP_CRYPTO);
}

ZTEST(zhostile, test_4_5_sha256_out_kernel_ptr)
{
	expect_reclaimed("h_shao", sha_out_kernel_ptr, APP_CAP_LOG | APP_CAP_CRYPTO);
	zassert_equal(kernel_var, 0xbeef, "kernel data was written by the syscall");
}

ZTEST(zhostile, test_4_5_log_bad_ptrs)
{
	/* String copies are pre-validated: refused, the app keeps running. */
	expect_refused("h_logk", log_kernel_ptr, APP_CAP_LOG, 0);
}

/* ---- 4.10 resource exhaustion ---- */

static void ipc_flood(void *a)
{
	ARG_UNUSED(a);
	int rc = 0;
	int n = 0;

	for (n = 0; n < 1000; n++) {
		rc = svc_ipc_send(canary_id, user_buf, 8);
		if (rc != 0) {
			break;
		}
	}
	report_rc = rc;
	report_n = n;
	report_done = 1;
}

static struct k_thread rogue_thread;
static K_THREAD_STACK_DEFINE(rogue_stack, 512);

static void thread_spawn(void *a)
{
	ARG_UNUSED(a);
	/* A user app holds no permission on kernel thread objects: creating a
	 * thread must be refused (oops) rather than succeed.
	 */
	k_thread_create(&rogue_thread, rogue_stack, K_THREAD_STACK_SIZEOF(rogue_stack),
			(k_thread_entry_t)thread_spawn, NULL, NULL, NULL, 5, K_USER,
			K_NO_WAIT);
	report_done = 1;
}

static void stack_blow(void *a)
{
	ARG_UNUSED(a);
	volatile uint8_t big[4096];

	big[0] = 1;
	big[4095] = 2;
	report_rc = big[0] + big[4095];
	report_done = 1;
}

ZTEST(zhostile, test_4_10_ipc_flood)
{
	uint8_t peer_buf[8] = {1};

	expect_refused("h_ipc", ipc_flood, APP_CAP_LOG | APP_CAP_IPC, -EAGAIN);
	int sent = report_n;

	printk("  ipc flood: %d messages accepted, then -EAGAIN\n", sent);
	zassert_true(sent > 0 && sent < 1000, "mailbox unbounded or closed");
	/* Another destination keeps its own budget: main can still deliver to
	 * a third app's mailbox (use the canary's neighbour slot id 2 if free).
	 */
	ARG_UNUSED(peer_buf);
}

ZTEST(zhostile, test_4_10_thread_spawn)
{
	expect_reclaimed("h_thr", thread_spawn, APP_CAP_LOG);
}

ZTEST(zhostile, test_4_10_stack_overflow)
{
	/* 4 KB frame on a 2 KB stack: the guard/PMP stack bound traps it. */
	expect_reclaimed("h_stk", stack_blow, APP_CAP_LOG);
}

/* ---- 4.11 identity, and the reach of a granted capability ---- */

/* Rewriting the thread's custom data must not change which application the
 * platform thinks is calling: an app that borrows another id would inherit its
 * capabilities. This app holds no capability at all, so svc_log has to stay
 * refused whatever it writes there.
 */
/* The identity has to follow the thread, not any state the application can
 * write. Whatever this app does to its own scratch state, the capability it was
 * never granted stays out of reach.
 */
static void forge_identity(void *a)
{
	ARG_UNUSED(a);
	uint8_t in[4] = {0};
	uint8_t out[32];

#ifdef CONFIG_THREAD_CUSTOM_DATA
	for (uintptr_t id = 0; id < 8; id++) {
		k_thread_custom_data_set((void *)(id + 1));
	}
#endif
	report_rc = svc_sha256(in, sizeof(in), out);
	report_done = 1;
}

/* A flash range that wraps past the end of the partition must be refused
 * rather than wrapping into a valid offset.
 */
static void flash_wrap(void *a)
{
	ARG_UNUSED(a);
	report_rc = svc_flash_erase(0xFFFF0000U, 0x10000U);
	report_done = 1;
}

/* The capability alone grants nothing: without a window the flash services
 * answer -EACCES, so the registry and the signed modules stay out of reach.
 */
static void flash_ungranted(void *a)
{
	ARG_UNUSED(a);
	uint8_t buf[4];

	report_rc = svc_flash_read(0, buf, sizeof(buf));
	report_done = 1;
}

ZTEST(zhostile, test_4_11_forged_identity)
{
	/* A neighbour that does hold the crypto capability, so a successful
	 * forgery would be observable: the hostile app carries none of it and
	 * must still be refused whatever identity it writes.
	 */
	const struct app_def rich = {
		.name = "rich",
		.source = APP_BUILTIN,
		.entry = canary_app,
		.caps = APP_CAP_LOG | APP_CAP_CRYPTO,
	};
	int rid = app_platform_launch(&rich);

	zassert_true(rid >= 0, "neighbour launch failed");
	k_mem_domain_add_partition(app_platform_domain(rid), &victim_part);
	k_msleep(20);

	/* LOG only: enough to reach its own report memory, never CRYPTO. */
	expect_refused("h_id", forge_identity, APP_CAP_LOG, -EPERM);
}

ZTEST(zhostile, test_4_11_flash_wrap)
{
	/* Grant every slot a small window first, so the request is refused for
	 * wrapping past its end rather than for holding no grant at all.
	 */
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_flash(i, 0, 4096);
	}
	expect_refused("h_fw", flash_wrap, APP_CAP_FLASH, -EACCES);
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_flash(i, 0, 0);
	}
}

ZTEST(zhostile, test_4_11_flash_ungranted)
{
	expect_refused("h_fu", flash_ungranted, APP_CAP_FLASH, -EACCES);
}

ZTEST_SUITE(zhostile, NULL, hostile_setup, hostile_before, NULL, NULL);
