/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A loadable app that calls the platform's shared service library directly. It
 * links no driver code and no per-sample service glue: the services, their
 * capability checks and their syscalls all live in the platform. The app proves
 * the shared library is reachable, capability-gated and usable from inside an
 * isolated llext module.
 */

#include <zephyr/app_platform/app_services.h>

#include <zephyr/llext/symbol.h>
#include <zephyr/kernel.h>

static void say(const char *label, int rc)
{
	char line[40];
	int i = 0;

	while (*label) {
		line[i++] = *label++;
	}
	line[i++] = ':'; line[i++] = ' ';
	if (rc == 0) {
		line[i++] = 'P'; line[i++] = 'A'; line[i++] = 'S'; line[i++] = 'S';
	} else {
		line[i++] = 'F'; line[i++] = 'A'; line[i++] = 'I'; line[i++] = 'L';
		line[i++] = ' '; line[i++] = 'r'; line[i++] = 'c'; line[i++] = '=';
		int v = rc < 0 ? -rc : rc;

		if (rc < 0) {
			line[i++] = '-';
		}
		if (v >= 100) {
			line[i++] = '0' + (v / 100); v %= 100;
		}
		if (v >= 10) {
			line[i++] = '0' + (v / 10);
		}
		line[i++] = '0' + (v % 10);
	}
	line[i] = '\0';
	svc_log(line);
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	svc_log("probe: calling the shared service library from an llext app");

	/* time group */
	say("uptime", svc_uptime_ms() > 0 ? 0 : -1);
	say("cycles", svc_cycles() > 0 ? 0 : -1);

	/* random group */
	uint8_t rnd[16] = {0};
	int rc = svc_random(rnd, sizeof(rnd));
	int nonzero = 0;

	for (int j = 0; j < 16; j++) {
		nonzero |= rnd[j];
	}
	say("random", (rc == 0 && nonzero) ? 0 : -1);

	/* crypto group */
	uint8_t digest[32] = {0};

	rc = svc_sha256((const uint8_t *)"hello", 5, digest);
	say("sha256", (rc == 0 && digest[0] != 0) ? 0 : -1);

	/* sensor group */
	say("die_temp", svc_die_temp_milli() > 0 ? 0 : -1);

	/* kv group: round-trip a value through the lock-serialized store */
	say("kv_set", svc_kv_set("count", 7));
	uint32_t got = 0;

	rc = svc_kv_get("count", &got);
	say("kv_get", (rc == 0 && got == 7) ? 0 : -1);

	/* flash group: read the storage partition into an app buffer */
	uint8_t fbuf[16] = {0};

	say("flash_read", svc_flash_read(0, fbuf, sizeof(fbuf)));

	/* gpio group: line 0 is granted, line 1 is not */
	say("gpio_count", svc_gpio_count() >= 2 ? 0 : -1);
	say("gpio_granted", svc_gpio_set(0, 1));
	say("gpio_denied", svc_gpio_set(1, 1) == -1 ? 0 : -1);

	/* ipc group: send to our own mailbox and read it back */
	const char *m = "ping";

	rc = svc_ipc_send(0, m, 5);
	uint8_t inbox[16] = {0};
	int got_len = svc_ipc_recv(inbox, sizeof(inbox), 100);

	say("ipc_send", rc);
	say("ipc_recv", (got_len == 5 && inbox[0] == 'p') ? 0 : -1);

	/* inspect group: read our own group's status */
	struct svc_app_info info;

	rc = svc_app_info(0, &info);
	say("app_count", svc_app_count() > 0 ? 0 : -1);
	say("app_info", (rc == 0 && info.active) ? 0 : -1);

	/* periph group: PWM channel 0 and ADC channel 0, both granted */
	say("pwm_set", svc_pwm_set(0, 500));
	say("adc_read", svc_adc_read(0) >= 0 ? 0 : -1);

	/*
	 * bus group: with no device wired we check the capability and grant
	 * path. A granted I2C address reaches the driver (not -EPERM); an
	 * ungranted address is refused. UART is granted by capability alone.
	 */
	uint8_t b[2] = {0};

	say("i2c_granted", svc_i2c_read(0x50, b, 1) != -1 ? 0 : -1);
	say("i2c_denied", svc_i2c_read(0x51, b, 1) == -1 ? 0 : -1);
	say("spi_xfer", svc_spi_xfer((const uint8_t *)"\x9f", b, 1) != -1 ? 0 : -1);
	/* uart: capability passes; no device wired returns a clean error, not a fault */
	say("uart_path", svc_uart_write((const uint8_t *)"hi", 2) != -1 ? 0 : -1);

	svc_log("probe: done - all from platform-owned services");
	for (;;) {
		svc_heartbeat();
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
