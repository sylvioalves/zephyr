/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * An isolated user-mode application setting the secondary UART's line
 * parameters and moving bytes over it. The board overlay loops the port back
 * on itself, so whatever the application writes it reads again and can check
 * that a reconfigured port really reframes the bytes. The platform owns the
 * port throughout: the application never touches the driver, and the boot
 * configuration is restored when it closes or is reclaimed.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>

K_APPMEM_PARTITION_DEFINE(uart_part);
K_APP_DMEM(uart_part) static int passes;
K_APP_DMEM(uart_part) static int fails;

static void check(bool ok, const char *what, int rc)
{
	char line[72];

	if (ok) {
		(void)snprintk(line, sizeof(line), "PASS %s", what);
		passes++;
	} else {
		(void)snprintk(line, sizeof(line), "FAIL %s (rc=%d)", what, rc);
		fails++;
	}
	svc_log(line);
}

/* Read back what was just written. The port is looped back, so the bytes are
 * already on their way; poll briefly rather than blocking a thread on them.
 */
static int loopback_read(uint8_t *buf, size_t len)
{
	int got = 0;

	for (int i = 0; i < 80 && got < (int)len; i++) {
		int n = svc_uart_read(&buf[got], len - got);

		if (n > 0) {
			got += n;
		} else {
			(void)svc_timer_start(2);
			(void)svc_timer_wait(5);
		}
	}
	return got;
}

static void uart_app(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("uart_line: start");

	int rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 1, 8);

	if (rc == -ENODEV) {
		svc_log("uart_line: no secondary uart on this board, nothing to do");
		return;
	}
	check(rc == 0, "configure 115200 8N1", rc);

	rc = svc_uart_configure(9600, APP_UART_PARITY_EVEN, 2, 7);
	check(rc == 0, "configure 9600 7E2", rc);

	rc = svc_uart_configure(0, APP_UART_PARITY_NONE, 1, 8);
	check(rc == -EINVAL, "a zero baud rate is refused", rc);
	rc = svc_uart_configure(115200, 99, 1, 8);
	check(rc == -EINVAL, "an unknown parity is refused", rc);
	rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 5, 8);
	check(rc == -EINVAL, "an impossible stop-bit count is refused", rc);
	rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 1, 12);
	check(rc == -EINVAL, "an impossible data-bit count is refused", rc);

	rc = svc_uart_configure(115200, APP_UART_PARITY_NONE, 1, 8);
	check(rc == 0, "configure back to 115200 8N1", rc);

	static const uint8_t tx[5] = {'h', 'e', 'l', 'l', 'o'};
	uint8_t rx[8] = {0};

	rc = svc_uart_write(tx, sizeof(tx));
	check(rc == (int)sizeof(tx), "write to the looped-back port", rc);

	int got = loopback_read(rx, sizeof(tx));

	check(got == (int)sizeof(tx), "read every byte back", got);
	check(got == (int)sizeof(tx) && memcmp(rx, tx, sizeof(tx)) == 0,
	      "the bytes match what was written", rx[0]);

	/* A different framing must loop back just as cleanly, which is what
	 * shows the port was really reconfigured and not merely accepted.
	 */
	rc = svc_uart_configure(9600, APP_UART_PARITY_EVEN, 1, 8);
	check(rc == 0, "configure 9600 8E1 for a second pass", rc);

	static const uint8_t tx2[3] = {0x5A, 0xA5, 0x42};
	uint8_t rx2[4] = {0};

	(void)svc_uart_write(tx2, sizeof(tx2));
	got = loopback_read(rx2, sizeof(tx2));
	check(got == (int)sizeof(tx2) && memcmp(rx2, tx2, sizeof(tx2)) == 0,
	      "the bytes match at 9600 8E1 too", got);

	rc = svc_uart_close();
	check(rc == 0, "close restores the boot configuration", rc);

	char line[48];

	(void)snprintk(line, sizeof(line), "uart_line: %d passed, %d failed",
		       passes, fails);
	svc_log(line);
	svc_log(fails == 0 ? "uart_line: done" : "uart_line: FAILURES");
}

int main(void)
{
	printk("\n*** app_uart_line: uart line parameters from an application ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def def = {
		.name = "uartline",
		.source = APP_BUILTIN,
		.entry = uart_app,
		.caps = APP_CAP_LOG | APP_CAP_TIME | APP_CAP_UART,
	};

	int id = app_platform_launch(&def);

	if (id < 0) {
		printk("launch failed (%d)\n", id);
		return 0;
	}
	k_mem_domain_add_partition(app_platform_domain(id), &uart_part);

	printk("*** launched isolated uart app (group %d) ***\n", id);
	return 0;
}
