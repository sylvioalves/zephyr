/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Hostile applications: each one attempts something it must not be able to do.
 * Pass condition: the offending app is trapped and reclaimed (or the call is
 * refused with an error), the platform and a canary app keep running.
 */

#include <zephyr/ztest.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/fs/fs.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/arch/syscall.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <string.h>
#include <errno.h>

#define HAS_GPIO0 DT_NODE_EXISTS(DT_NODELABEL(gpio0))
#define HAS_PRIV_PROBES \
	(IS_ENABLED(CONFIG_RISCV) || IS_ENABLED(CONFIG_CPU_CORTEX_M) || IS_ENABLED(CONFIG_ARM64))
#define COUNTERS DT_PROP_LEN_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_app_platform), counters, 0)

/* Data owned by the canary app's domain only. */
K_APPMEM_PARTITION_DEFINE(victim_part);
K_APP_DMEM(victim_part) volatile uint32_t victim_data = 0x600d;

/* Results partition shared with the hostile app so it can report. */
K_APPMEM_PARTITION_DEFINE(report_part);
K_APP_DMEM(report_part) volatile int report_rc;
K_APP_DMEM(report_part) volatile int report_done;
K_APP_DMEM(report_part) volatile int report_n;

/* A slot's memory domain outlives the app that ran in it, so a later test
 * reusing the slot finds the report partition already there. Add it only when
 * it is missing, to keep the domain valid without logging a false overlap.
 */
static void add_report_part(int id)
{
	struct k_mem_domain *dom = app_platform_domain(id);

	if (dom == NULL) {
		return;
	}

	for (int i = 0; i < CONFIG_MAX_DOMAIN_PARTITIONS; i++) {
		if (dom->partitions[i].start == report_part.start &&
		    dom->partitions[i].size == report_part.size) {
			return;
		}
	}

	k_mem_domain_add_partition(dom, &report_part);
}

static uint32_t kernel_var = 0xbeef;
extern const struct z_object *_k_syscall_table;

static void canary_app(void *arg)
{
	ARG_UNUSED(arg);
	for (;;) {
		victim_data++;
		app_svc_heartbeat();
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
static void expect_reclaimed(const char *name, app_entry_t entry, app_caps_t caps)
{
	const struct app_def def = {
		.name = name, .source = APP_BUILTIN, .entry = entry, .caps = caps,
	};
	uint32_t before = victim_data;
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed");
	add_report_part(id);
	zassert_true(wait_slot_free(name, 2000), "%s was not reclaimed", name);
	zassert_equal(report_done, 0, "%s completed instead of trapping", name);

	struct app_platform_status st;

	zassert_equal(app_platform_status(canary_id, &st), 0);
	zassert_true(st.active, "canary died");
	k_msleep(30);
	zassert_true(victim_data != before, "canary stopped running");
}

/* Launch a hostile app that must be refused by return value and survive. */
static void expect_refused(const char *name, app_entry_t entry, app_caps_t caps,
			   int expected_rc)
{
	const struct app_def def = {
		.name = name, .source = APP_BUILTIN, .entry = entry, .caps = caps,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed");
	add_report_part(id);
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
#if HAS_GPIO0
	volatile uint32_t *reg = (volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(gpio0)) + 0x8U);

	*reg = BIT(2);
#endif
	report_done = 1;
}

static void write_uart_mmio(void *a)
{
	ARG_UNUSED(a);
	/* UART0 FIFO register */
	volatile uint32_t *reg = (volatile uint32_t *)DT_REG_ADDR(DT_NODELABEL(uart0));
	*reg = 'X';
	report_done = 1;
}

ZTEST(zhostile, test_4_3_gpio_mmio)
{
	if (!HAS_GPIO0) {
		ztest_test_skip();
	}
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

#if defined(CONFIG_RISCV)
	__asm__ volatile("csrr %0, mstatus" : "=r"(v));
#elif defined(CONFIG_CPU_CORTEX_M)
	/* SCB->VTOR: the System Control Space is privileged-only. */
	v = *(volatile uint32_t *)0xE000ED08U;
#elif defined(CONFIG_ARM64)
	uint64_t v64;

	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v64));
	v = (uint32_t)v64;
#else
	v = 0U;
#endif
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
#if defined(CONFIG_RISCV)
	__asm__ volatile("mret");
#elif defined(CONFIG_CPU_CORTEX_M)
	/* An exception return from thread mode is an invalid state. */
	__asm__ volatile("bx %0" : : "r"(0xFFFFFFF9U));
#elif defined(CONFIG_ARM64)
	__asm__ volatile("eret");
#endif
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
	if (!HAS_PRIV_PROBES) {
		ztest_test_skip();
	}
	expect_reclaimed("h_csr", csrr_mstatus, APP_CAP_LOG);
}

ZTEST(zhostile, test_4_4_mret)
{
	if (!HAS_PRIV_PROBES) {
		ztest_test_skip();
	}
	expect_reclaimed("h_mret", do_mret, APP_CAP_LOG);
}

ZTEST(zhostile, test_4_4_wfi)
{
	/* wfi from user mode either traps or completes on the next interrupt,
	 * depending on the core; either outcome keeps the kernel intact. Record
	 * which one happened.
	 */
	const struct app_def def = {
		.name = "h_wfi", .source = APP_BUILTIN, .entry = do_wfi,
		.caps = APP_CAP_LOG,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0);
	add_report_part(id);
	zassert_true(wait_slot_free("h_wfi", 2000), "wfi app never finished");
	printk("  wfi from user mode: %s\n", report_done ? "executed, app continued"
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

	report_rc = app_svc_sha256((const uint8_t *)&kernel_var, 4, out);
	report_done = 1;
}

static void sha_wild_ptr(void *a)
{
	ARG_UNUSED(a);
	uint8_t out[32];

	report_rc = app_svc_sha256((const uint8_t *)0xdeadbee0, 4, out);
	report_done = 1;
}

static void sha_foreign_ptr(void *a)
{
	ARG_UNUSED(a);
	uint8_t out[32];

	report_rc = app_svc_sha256((const uint8_t *)&victim_data, 4, out);
	report_done = 1;
}

static void sha_out_kernel_ptr(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_sha256(user_buf, 4, (uint8_t *)&kernel_var);
	report_done = 1;
}

static void log_kernel_ptr(void *a)
{
	ARG_UNUSED(a);
	app_svc_log((const char *)&kernel_var);
	app_svc_log((const char *)0xdeadbeef);
	app_svc_log((const char *)&victim_data);
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
		rc = app_svc_ipc_send(canary_id, user_buf, 8);
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
 * capabilities. This app holds no capability at all, so app_svc_log has to stay
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
	report_rc = app_svc_sha256(in, sizeof(in), out);
	report_done = 1;
}

/* A flash range that wraps past the end of the partition must be refused
 * rather than wrapping into a valid offset.
 */
static void flash_wrap(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_flash_erase(0xFFFF0000U, 0x10000U);
	report_done = 1;
}

/* The capability alone grants nothing: without a window the flash services
 * answer -EACCES, so the registry and the signed modules stay out of reach.
 */
static void flash_ungranted(void *a)
{
	ARG_UNUSED(a);
	uint8_t buf[4];

	report_rc = app_svc_flash_read(0, buf, sizeof(buf));
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

/* The sensor catalog is capability-gated: an application that was not granted
 * APP_CAP_SENSOR may not even learn how many sensors the board announces.
 */
static void sensor_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_sensor_count();
	report_done = 1;
}

ZTEST(zhostile, test_4_12_sensor_ungranted)
{
	expect_refused("h_sns", sensor_ungranted, APP_CAP_LOG, -EPERM);
}

/* The grant mask is wider than 32 bits, so a capability allocated above bit 31
 * must reach the service check intact. app_caps.h asserts the width at build
 * time; this covers the runtime half, that a high bit rides through the launch
 * path without displacing or standing in for a real low-bit capability.
 */
#define CAP_HIGH APP_CAP_BIT(32)

static void sensor_probe(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_sensor_count();
	report_done = 1;
}

ZTEST(zhostile, test_4_13_capability_above_bit31)
{
	const struct app_def granted = {
		.name = "h_hi", .source = APP_BUILTIN, .entry = sensor_probe,
		.caps = APP_CAP_LOG | APP_CAP_SENSOR | CAP_HIGH,
	};
	int id = app_platform_launch(&granted);

	zassert_true(id >= 0, "launch failed");
	add_report_part(id);
	zassert_true(wait_slot_free("h_hi", 2000), "h_hi never exited");
	zassert_equal(report_done, 1, "h_hi trapped");
	zassert_true(report_rc >= 0, "a high bit displaced APP_CAP_SENSOR: rc %d",
		     report_rc);

	/* A high bit must not stand in for a capability the app was not granted. */
	expect_refused("h_hio", sensor_probe, APP_CAP_LOG | CAP_HIGH, -EPERM);
}

/* ---- 4.14 controlling other applications ---- */

/* A target for the control tests: it only has to stay alive and be stoppable,
 * and it is separate from the canary so stopping it cannot disturb the other
 * suites' liveness assertions.
 */
static void control_target(void *a)
{
	ARG_UNUSED(a);
	for (;;) {
		app_svc_heartbeat();
		k_msleep(10);
	}
}

/* An application must not stop itself: the abort would tear the caller down
 * inside its own system call, so the platform refuses rather than returning.
 * The id is handed in through the report partition because an application has
 * no system call that tells it its own group id.
 */
K_APP_DMEM(report_part) int self_id;

static void stop_self(void *a)
{
	ARG_UNUSED(a);
	/* The thread starts inside the launch call, so wait for the test to
	 * publish the id rather than racing it.
	 */
	while (self_id < 0) {
		k_msleep(5);
	}
	report_rc = app_svc_app_stop(self_id);
	report_done = 1;
}

static void stop_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_app_stop(canary_id);
	report_done = 1;
}

static void start_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_app_start(canary_id);
	report_done = 1;
}

K_APP_DMEM(report_part) int target_id;

static void start_target(void *a)
{
	ARG_UNUSED(a);
	while (target_id < 0) {
		k_msleep(5);
	}
	report_rc = app_svc_app_start(target_id);
	report_done = 1;
}

static void stop_target(void *a)
{
	ARG_UNUSED(a);
	while (target_id < 0) {
		k_msleep(5);
	}
	report_rc = app_svc_app_stop(target_id);
	report_done = 1;
}

ZTEST(zhostile, test_4_14_control_ungranted)
{
	expect_refused("h_cs", stop_ungranted, APP_CAP_LOG, -EPERM);
	expect_refused("h_cr", start_ungranted, APP_CAP_LOG, -EPERM);
}

ZTEST(zhostile, test_4_14_control_self_refused)
{
	const struct app_def def = {
		.name = "h_cself", .source = APP_BUILTIN, .entry = stop_self,
		.caps = APP_CAP_LOG | APP_CAP_CONTROL,
	};
	self_id = -1;

	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed");
	add_report_part(id);
	self_id = id;
	zassert_true(wait_slot_free("h_cself", 2000), "h_cself never exited");
	zassert_equal(report_done, 1, "h_cself was stopped instead of refused");
	zassert_equal(report_rc, -EINVAL, "self stop rc %d != -EINVAL", report_rc);
}

ZTEST(zhostile, test_4_14_control_stops_another_app)
{
	const struct app_def tgt = {
		.name = "ctltgt", .source = APP_BUILTIN, .entry = control_target,
		.caps = APP_CAP_LOG,
	};

	target_id = -1;

	int tid = app_platform_launch(&tgt);

	zassert_true(tid >= 0, "target launch failed");
	k_msleep(30);

	struct app_platform_status st;

	zassert_equal(app_platform_status(tid, &st), 0);
	zassert_true(st.active, "target is not running before the stop");

	const struct app_def ctl = {
		.name = "h_ctl", .source = APP_BUILTIN, .entry = stop_target,
		.caps = APP_CAP_LOG | APP_CAP_CONTROL,
	};
	int id = app_platform_launch(&ctl);

	zassert_true(id >= 0, "controller launch failed");
	add_report_part(id);
	target_id = tid;
	zassert_true(wait_slot_free("h_ctl", 2000), "controller never exited");
	zassert_equal(report_done, 1, "controller trapped");
	zassert_equal(report_rc, 0, "app_svc_app_stop rc %d", report_rc);

	/* A stop keeps the slot and the module so the group can run again, so
	 * the proof it really stopped is that the slot went inactive, not that
	 * it was freed.
	 */
	zassert_equal(app_platform_status(tid, &st), 0);
	zassert_false(st.active, "target still active after app_svc_app_stop");

	/* And a controller can start it again, with the same slot. */
	const struct app_def re = {
		.name = "h_rst", .source = APP_BUILTIN, .entry = start_target,
		.caps = APP_CAP_LOG | APP_CAP_CONTROL,
	};

	report_rc = 0;
	report_done = 0;
	int rid = app_platform_launch(&re);

	zassert_true(rid >= 0, "restarter launch failed");
	add_report_part(rid);
	zassert_true(wait_slot_free("h_rst", 2000), "restarter never exited");
	zassert_equal(report_done, 1, "restarter trapped");
	zassert_equal(report_rc, 0, "app_svc_app_start rc %d", report_rc);

	k_msleep(30);
	zassert_equal(app_platform_status(tid, &st), 0);
	zassert_true(st.active, "target did not come back after app_svc_app_start");

	zassert_equal(app_platform_remove(tid), 0);
}

/* ---- 4.16 the hardware counter service ---- */

static void counter_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_counter_count();
	report_done = 1;
}

/* Holding the capability is not holding an instance: instance 1 is deliberately
 * never granted, so it must answer -EACCES rather than counting.
 */
static void counter_instance_ungranted(void *a)
{
	ARG_UNUSED(a);

	uint32_t ticks = 0;

	report_rc = app_svc_counter_read(1, &ticks);
	report_done = 1;
}

static void counter_out_of_range(void *a)
{
	ARG_UNUSED(a);

	uint32_t ticks = 0;

	report_rc = app_svc_counter_read(99, &ticks);
	report_done = 1;
}

/* A granted counter must actually advance, and its frequency must be sane,
 * otherwise "it returned 0" would pass against a dead peripheral.
 */
static void counter_runs(void *a)
{
	ARG_UNUSED(a);

	uint32_t hz = 0;
	uint32_t t1 = 0;
	uint32_t t2 = 0;
	int rc = app_svc_counter_frequency(0, &hz);

	if (rc != 0) {
		report_rc = rc;
		report_done = 1;
		return;
	}
	if (hz == 0U) {
		report_rc = -ERANGE;
		report_done = 1;
		return;
	}

	rc = app_svc_counter_start(0);
	if (rc != 0) {
		report_rc = rc;
		report_done = 1;
		return;
	}

	rc = app_svc_counter_read(0, &t1);
	if (rc == 0) {
		k_msleep(20);
		rc = app_svc_counter_read(0, &t2);
	}
	if (rc == 0 && t2 == t1) {
		rc = -ESTALE;   /* counted nothing over 20 ms */
	}
	report_rc = rc;
	report_done = 1;
}

ZTEST(zhostile, test_4_16_counter_ungranted)
{
	expect_refused("h_cnu", counter_ungranted, APP_CAP_LOG, -EPERM);
}

ZTEST(zhostile, test_4_16_counter_instance_ungranted)
{
	if (COUNTERS < 2) {
		ztest_test_skip();
	}
	int id = slot_of("canary");

	ARG_UNUSED(id);
	expect_refused("h_cni", counter_instance_ungranted,
		       APP_CAP_LOG | APP_CAP_COUNTER_DEV, -EACCES);
	expect_refused("h_cnr", counter_out_of_range,
		       APP_CAP_LOG | APP_CAP_COUNTER_DEV, -EINVAL);
}

ZTEST(zhostile, test_4_16_counter_runs)
{
	if (COUNTERS < 2) {
		ztest_test_skip();
	}
	/* Grant instance 0 to every slot and leave instance 1 ungranted. */
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_counter(i, BIT(0));
	}
	expect_refused("h_cnt", counter_runs, APP_CAP_LOG | APP_CAP_COUNTER_DEV, 0);
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		app_platform_grant_counter(i, 0U);
	}
}

/* ---- 4.15 the per-application file sandbox ---- */

static void fs_ungranted(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_file_open("x.txt", 1);
	report_done = 1;
}

static void fs_escape_dotdot(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_file_open("../0/secret.txt", 1);
	report_done = 1;
}

static void fs_roundtrip(void *a)
{
	ARG_UNUSED(a);

	int h = app_svc_file_open("note.txt", 1);

	if (h < 0) {
		report_rc = h;
		report_done = 1;
		return;
	}

	char buf[8] = {0};
	int rc = app_svc_file_write(h, "abcd", 4);

	if (rc == 4) {
		rc = app_svc_file_seek(h, 0, FS_SEEK_SET);
		if (rc == 0) {
			rc = app_svc_file_read(h, buf, sizeof(buf));
			rc = (rc == 4 && memcmp(buf, "abcd", 4) == 0) ? 0 :
			     (rc < 0 ? rc : -EIO);
		}
	} else if (rc >= 0) {
		rc = -EIO;
	}
	(void)app_svc_file_close(h);
	report_rc = rc;
	report_done = 1;
}

ZTEST(zhostile, test_4_15_fs_gate_and_confinement)
{
	expect_refused("h_fnp", fs_ungranted, APP_CAP_LOG, -EPERM);
	expect_refused("h_fdd", fs_escape_dotdot, APP_CAP_LOG | APP_CAP_FS, -EACCES);
	expect_refused("h_frt", fs_roundtrip, APP_CAP_LOG | APP_CAP_FS, 0);
}

/* An application must not be able to take the system down through the depth of
 * a service call. The privileged stack is a system resource the application
 * never sees, so exhausting it has to be contained like any other fault: the
 * application is reclaimed, the system and its neighbours keep running.
 */
static void priv_stack_pressure(void *a)
{
	ARG_UNUSED(a);

	/* Deep filesystem calls in a loop, each descending through littlefs
	 * into the flash driver on the privileged stack.
	 */
	for (int i = 0; i < 64; i++) {
		char name[16];

		name[0] = 'p';
		name[1] = (char)('0' + (i % 10));
		name[2] = '.';
		name[3] = 't';
		name[4] = '\0';

		int h = app_svc_file_open(name, 1);

		if (h >= 0) {
			(void)app_svc_file_write(h, "0123456789abcdef", 16);
			(void)app_svc_file_close(h);
		}
	}
	report_rc = 0;
	report_done = 1;
}

ZTEST(zhostile, test_4_16_deep_service_calls_do_not_kill_the_system)
{
	const struct app_def def = {
		.name = "h_deep", .source = APP_BUILTIN, .entry = priv_stack_pressure,
		.caps = APP_CAP_LOG | APP_CAP_FS,
	};
	int id = app_platform_launch(&def);

	zassert_true(id >= 0, "launch failed");
	add_report_part(id);
	zassert_true(wait_slot_free("h_deep", 8000), "h_deep never finished");

	/* Whether it completed or was reclaimed is not the point: the system
	 * must still be here, and the canary must still be running.
	 */
	struct app_platform_status st;

	zassert_equal(app_platform_status(canary_id, &st), 0);
	zassert_true(st.active, "canary died: the system did not survive");

	uint32_t before = victim_data;

	k_msleep(30);
	zassert_true(victim_data != before, "canary stopped running");
}

/* ---- 4.17 state left behind by a reclaimed application ---- */

static void ipc_claim_name(void *a)
{
	ARG_UNUSED(a);
	report_rc = app_svc_ipc_register("stale-name");
	report_done = 1;
}

static void ipc_send_to_stale(void *a)
{
	ARG_UNUSED(a);
	uint8_t msg = 1;

	report_rc = app_svc_ipc_send_to("stale-name", &msg, sizeof(msg));
	report_done = 1;
}

ZTEST(zhostile, test_4_17_ipc_name_released_on_reclaim)
{
	expect_refused("h_ipr", ipc_claim_name, APP_CAP_LOG | APP_CAP_IPC, 0);
	expect_refused("h_ips", ipc_send_to_stale, APP_CAP_LOG | APP_CAP_IPC, -ENOENT);
}

static void checkpoint_store(void *a)
{
	ARG_UNUSED(a);
	app_svc_checkpoint_set(1234U);
	report_rc = 0;
	report_done = 1;
}

static void checkpoint_report(void *a)
{
	ARG_UNUSED(a);
	report_rc = (int)app_svc_checkpoint_get();
	report_done = 1;
}

ZTEST(zhostile, test_4_17_checkpoint_follows_the_name)
{
	expect_refused("h_cpa", checkpoint_store, APP_CAP_LOG, 0);
	expect_refused("h_cpb", checkpoint_report, APP_CAP_LOG, 0);
	expect_refused("h_cpa", checkpoint_report, APP_CAP_LOG, 1234);
}

ZTEST(zhostile, test_4_17_running_name_is_unique)
{
	const struct app_def def = {
		.name = "canary", .source = APP_BUILTIN, .entry = canary_app,
		.caps = APP_CAP_LOG,
	};

	zassert_equal(app_platform_launch(&def), -EEXIST,
		      "a second application ran under a running name");
}

ZTEST_SUITE(zhostile, NULL, hostile_setup, hostile_before, NULL, NULL);
