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

	/* flash group: erase, write, read back the storage partition */
	uint8_t fbuf[16] = {0};

	say("flash_read", svc_flash_read(0, fbuf, sizeof(fbuf)));
	say("flash_erase", svc_flash_erase(0, 4096));
	const uint8_t fw[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	svc_flash_write(0, fw, 4);
	uint8_t fr[4] = {0};

	svc_flash_read(0, fr, 4);
	say("flash_wr", (fr[0] == 0xDE && fr[3] == 0xEF) ? 0 : -1);

	/* hwinfo: device id and reset cause */
	uint8_t devid[12] = {0};

	say("device_id", svc_device_id(devid, sizeof(devid)) > 0 ? 0 : -1);
	uint32_t cause = 0;

	say("reset_cause", svc_reset_cause(&cause) == 0 ? 0 : -1);

	/* timer: arm 100ms and wait for it */
	svc_timer_start(100);
	say("timer", svc_timer_wait(500) == 0 ? 0 : -1);

	/* rtc: seed the wall clock and read it back */
	svc_rtc_set(1000000);
	uint32_t epoch = 0;

	svc_rtc_now(&epoch);
	say("rtc", epoch >= 1000000 ? 0 : -1);

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
	uint8_t urx[4] = {0};

	say("uart_read", svc_uart_read(urx, sizeof(urx)) != -1 ? 0 : -1);

#ifndef PROBE_NO_BLE
	/* ble scan: see a peer board advertising the heart-rate service */
	svc_log("ble: scanning for HR peer (up to 4s)...");
	say("ble_scan", svc_ble_scan("HR-Peer", 4000) == 1 ? 0 : -1);

	/* ble gatt: connect to the HR peer and read one measurement */
	svc_log("ble: connecting + reading heart rate (up to 8s)...");
	uint8_t bpm = 0;
	int hr = svc_ble_hr_read(8000, &bpm);

	say("ble_hr", (hr == 0 && bpm > 0) ? 0 : -1);
#endif

	/*
	 * radio group: send to the 802.15.4 peer and read back its echo. BLE and
	 * 802.15.4 share one radio on this SoC and cannot run at once, so this
	 * runs only in the radio-only build (BLE compiled out).
	 */
#ifdef PROBE_NO_BLE
	svc_log("radio: sending to 802.15.4 peer, awaiting echo...");
	uint8_t rx[16] = {0};

	say("radio_send", svc_radio_send("hello-15.4", 10));
	int rn = svc_radio_recv(rx, sizeof(rx), 3000);

	say("radio_echo", (rn == 10 && rx[0] == 'h') ? 0 : -1);
#endif

	svc_log("probe: done - all from platform-owned services");
	for (;;) {
		svc_heartbeat();
		k_msleep(1000);
	}
}
EXPORT_SYMBOL(app_main);
