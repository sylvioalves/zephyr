/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Exercises the services an application gained most recently: several timer
 * slots at once, the UART line parameters, the BLE notify and advertising
 * calls, and polling the input queue without blocking. Each check prints PASS
 * or FAIL with what it observed, so a run on hardware either shows a clean
 * sheet or names the call that broke.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>

/* The counters are written by the application in user mode, so they live in a
 * partition added to its domain rather than in ordinary globals, which the
 * domain does not cover.
 */
K_APPMEM_PARTITION_DEFINE(newsvc_part);
K_APP_DMEM(newsvc_part) static int passes;
K_APP_DMEM(newsvc_part) static int fails;

static void check(bool ok, const char *what)
{
	char line[72];

	(void)snprintk(line, sizeof(line), "%s %s", ok ? "PASS" : "FAIL", what);
	svc_log(line);
	if (ok) {
		passes++;
	} else {
		fails++;
	}
}

/* Report a failure together with the value that caused it, so a bad return is
 * visible in the log rather than only the fact that it was wrong.
 */
static void check_rc(bool ok, const char *what, int rc)
{
	char line[72];

	if (ok) {
		check(true, what);
		return;
	}
	(void)snprintk(line, sizeof(line), "%s (rc=%d)", what, rc);
	check(false, line);
}

static void test_timers(void)
{
	svc_log("--- timer slots ---");

	int a = svc_timer_create();
	int b = svc_timer_create();

	check_rc(a >= 0, "timer_create first slot", a);
	check_rc(b >= 0 && b != a, "timer_create second, distinct slot", b);

	/* The point of slots: two timers run at once and each is waited on
	 * separately, which the single per-app timer could not do.
	 */
	int rc = svc_timer_arm(a, 120, 0);

	check_rc(rc == 0, "timer_arm slot a 120ms", rc);
	rc = svc_timer_arm(b, 40, 0);
	check_rc(rc == 0, "timer_arm slot b 40ms", rc);

	rc = svc_timer_slot_wait(b, 500);
	check_rc(rc == 0, "slot b fired", rc);

	/* b is the shorter timer, so a must still be pending here. */
	rc = svc_timer_slot_wait(a, 0);
	check_rc(rc == -EAGAIN, "slot a still pending, poll returns EAGAIN", rc);

	rc = svc_timer_slot_wait(a, 500);
	check_rc(rc == 0, "slot a fired after b", rc);

	int el = svc_timer_elapsed(a);

	check_rc(el >= 100 && el < 400, "timer_elapsed in range for slot a", el);

	rc = svc_timer_arm(a, 50, 1);
	check_rc(rc == 0, "timer_arm periodic", rc);
	rc = svc_timer_slot_wait(a, 400);
	check_rc(rc == 0, "periodic fired once", rc);
	rc = svc_timer_slot_wait(a, 400);
	check_rc(rc == 0, "periodic fired again without re-arming", rc);

	rc = svc_timer_stop(a);
	check_rc(rc == 0, "timer_stop", rc);

	rc = svc_timer_slot_wait(a, 150);
	check_rc(rc == -EAGAIN, "stopped slot does not fire", rc);

	rc = svc_timer_arm(99, 10, 0);
	check_rc(rc == -EINVAL, "out-of-range slot refused", rc);

	rc = svc_timer_free(a);
	check_rc(rc == 0, "timer_free slot a", rc);
	rc = svc_timer_arm(a, 10, 0);
	check_rc(rc == -EINVAL, "freed slot refused until re-created", rc);
	rc = svc_timer_free(b);
	check_rc(rc == 0, "timer_free slot b", rc);

	/* Every slot claimed then released, so the pool must come back whole. */
	int got = 0;

	for (int i = 0; i < APP_TIMER_SLOTS; i++) {
		if (svc_timer_create() >= 0) {
			got++;
		}
	}
	check_rc(got == APP_TIMER_SLOTS, "all slots claimable after free", got);

	rc = svc_timer_create();
	check_rc(rc == -ENOSPC, "exhausted pool returns ENOSPC", rc);

	for (int i = 0; i < APP_TIMER_SLOTS; i++) {
		(void)svc_timer_free(i);
	}
}

static void test_uart(void)
{
	svc_log("--- uart configure ---");

	int rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 1, 8);

	/* No secondary UART is chosen on this board, so ENODEV is the correct
	 * answer and proves the call reached the service rather than faulting.
	 */
	if (rc == -ENODEV) {
		check(true, "uart_configure reports ENODEV without a chosen port");
		rc = svc_uart_close();
		check_rc(rc == -ENODEV, "uart_close likewise", rc);
		return;
	}

	check_rc(rc == 0, "uart_configure 115200 8N1", rc);

	rc = svc_uart_configure(9600, APP_UART_PARITY_EVEN, 2, 7);
	check_rc(rc == 0, "uart_configure 9600 7E2", rc);

	rc = svc_uart_configure(0, APP_UART_PARITY_NONE, 1, 8);
	check_rc(rc == -EINVAL, "zero baud refused", rc);

	rc = svc_uart_configure(115200, 99, 1, 8);
	check_rc(rc == -EINVAL, "bad parity refused", rc);

	rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 5, 8);
	check_rc(rc == -EINVAL, "bad stop bits refused", rc);

	rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 1, 12);
	check_rc(rc == -EINVAL, "bad data bits refused", rc);

	/* Back to the rate the loopback pad is wired for before moving bytes. */
	rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 1, 8);
	check_rc(rc == 0, "uart_configure back to 115200 8N1", rc);

	/* TX and RX share one pad, so whatever is written comes straight back
	 * and the read must return the same bytes.
	 */
	static const uint8_t tx[5] = {'n', 'e', 'w', 's', 'v'};
	uint8_t rx[8] = {0};

	rc = svc_uart_write(tx, sizeof(tx));
	check_rc(rc == (int)sizeof(tx), "uart_write on the loopback port", rc);

	int got = 0;

	for (int i = 0; i < 50 && got < (int)sizeof(tx); i++) {
		int n = svc_uart_read(&rx[got], sizeof(rx) - got);

		if (n > 0) {
			got += n;
		} else {
			(void)svc_timer_start(2);
			(void)svc_timer_wait(5);
		}
	}
	check_rc(got == (int)sizeof(tx), "uart_read got every byte back", got);
	check_rc(got == (int)sizeof(tx) && memcmp(rx, tx, sizeof(tx)) == 0,
		 "loopback bytes match what was written", rx[0]);

	/* A different framing must still loop back cleanly, which is the point
	 * of being able to configure the port at all.
	 */
	rc = svc_uart_configure(9600, APP_UART_PARITY_EVEN, 1, 8);
	check_rc(rc == 0, "uart_configure 9600 8E1 for a second pass", rc);

	static const uint8_t tx2[3] = {0x5A, 0xA5, 0x42};
	uint8_t rx2[4] = {0};

	(void)svc_uart_write(tx2, sizeof(tx2));
	got = 0;
	for (int i = 0; i < 80 && got < (int)sizeof(tx2); i++) {
		int n = svc_uart_read(&rx2[got], sizeof(rx2) - got);

		if (n > 0) {
			got += n;
		} else {
			(void)svc_timer_start(2);
			(void)svc_timer_wait(5);
		}
	}
	check_rc(got == (int)sizeof(tx2) && memcmp(rx2, tx2, sizeof(tx2)) == 0,
		 "loopback matches at 9600 8E1 too", got);

	rc = svc_uart_close();
	check_rc(rc == 0, "uart_close restores the boot configuration", rc);
}

static const uint8_t svc_uuid[16] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t chrc0_uuid[16] = {
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
};
static const uint8_t chrc1_uuid[16] = {
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
};

static void test_ble(void)
{
	svc_log("--- ble notify and advertising ---");

	struct app_ble_chrc chrc[2] = {
		{.uuid_len = 16, .readable = 1, .writable = 0, .notifiable = 1},
		{.uuid_len = 16, .readable = 1, .writable = 1, .notifiable = 0},
	};

	for (int i = 0; i < 16; i++) {
		chrc[0].uuid[i] = chrc0_uuid[i];
		chrc[1].uuid[i] = chrc1_uuid[i];
	}

	int rc = svc_ble_serve_start("c5-newsvc", svc_uuid, sizeof(svc_uuid),
				     chrc, 2);

	check_rc(rc == 0, "serve_start with a notifiable characteristic", rc);
	if (rc != 0) {
		return;
	}

	rc = svc_ble_is_connected();
	check_rc(rc == 0, "is_connected reports no peer yet", rc);

	uint8_t val[4] = {1, 2, 3, 4};

	/* Nobody has subscribed, so a notify must be refused rather than
	 * queued: that is what keeps an unread app from building a backlog.
	 */
	rc = svc_ble_notify(0, val, sizeof(val));
	check_rc(rc == -ENOTCONN, "notify without a subscriber returns ENOTCONN",
		 rc);

	rc = svc_ble_notify(1, val, sizeof(val));
	check_rc(rc == -EACCES, "notify on a non-notifiable chrc refused", rc);

	rc = svc_ble_notify(9, val, sizeof(val));
	check_rc(rc == -EINVAL, "notify on a bad index refused", rc);

	rc = svc_ble_advertise(0, 0);
	check_rc(rc == 0, "advertise off", rc);

	rc = svc_ble_advertise(1, 200);
	check_rc(rc == 0, "advertise on at 200ms", rc);

	/* The value path must still work while serving, and a notify-capable
	 * characteristic is readable like any other.
	 */
	rc = svc_ble_serve_set(0, val, sizeof(val));
	check_rc(rc == 0, "serve_set on the notifiable characteristic", rc);

	uint8_t back[4] = {0};

	rc = svc_ble_serve_get(0, back, sizeof(back));
	check_rc(rc == (int)sizeof(val) && back[3] == 4,
		 "serve_get returns what was set", rc);

	svc_log("ble: advertising as 'c5-newsvc', connect to test notify");
}

static void test_input_poll(void)
{
	svc_log("--- non-blocking input ---");

	struct app_event evt;
	int rc = svc_event_wait(&evt, 0);

	check_rc(rc == -EAGAIN || rc == 0,
		 "event_wait(0) returns at once rather than blocking", rc);

	uint32_t t0 = svc_uptime_ms();

	for (int i = 0; i < 20; i++) {
		(void)svc_event_wait(&evt, 0);
	}

	uint32_t dt = svc_uptime_ms() - t0;

	/* Twenty polls that each blocked even briefly would take far longer;
	 * this is what distinguishes polling from a zero-length wait.
	 */
	check_rc(dt < 50, "20 polls complete promptly", (int)dt);

	rc = svc_button_wait(0);
	check_rc(rc == -EAGAIN || rc > 0,
		 "button_wait(0) polls instead of blocking", rc);
}

static void newsvc_app(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("newsvc: start");

	test_timers();
	test_uart();
	test_input_poll();
	test_ble();

	char line[64];

	(void)snprintk(line, sizeof(line), "newsvc: %d passed, %d failed",
		       passes, fails);
	svc_log(line);
	svc_log(fails == 0 ? "newsvc: ALL PASS" : "newsvc: FAILURES PRESENT");

	/* Stay alive so a phone can connect and exercise notify by hand. */
	for (;;) {
		struct app_event evt;

		if (svc_event_wait(&evt, 1000) == 0) {
			if (evt.kind == APP_EVENT_BLE_CONNECTED) {
				svc_log("newsvc: EVENT ble connected");
			} else if (evt.kind == APP_EVENT_BLE_DISCONNECTED) {
				svc_log("newsvc: EVENT ble disconnected");
			} else if (evt.kind == APP_EVENT_GATT_WRITE) {
				svc_log("newsvc: EVENT gatt write");
			}
		}

		if (svc_ble_is_connected() == 1) {
			static uint8_t tick;
			uint8_t v[2] = {0xAA, tick++};
			int rc = svc_ble_notify(0, v, sizeof(v));

			if (rc == 0) {
				svc_log("newsvc: notify delivered to subscriber");
			}
		}
	}
}

int main(void)
{
	printk("\n*** app_newsvc: timers, uart, ble notify, input polling ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "newsvc",
		.source = APP_BUILTIN,
		.entry = newsvc_app,
		.caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_UART |
			APP_CAP_BLE | APP_CAP_INPUT,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}
	k_mem_domain_add_partition(app_platform_domain(id), &newsvc_part);
	app_platform_grant_ble_gatt(id, svc_uuid, sizeof(svc_uuid));

	printk("*** launched newsvc app (group %d) ***\n", id);
	return 0;
}
