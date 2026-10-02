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

#include <zephyr/app_platform/app_sdk.h>

#include <errno.h>

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
	app_svc_log(line);
}

void app_main(void *arg)
{
	ARG_UNUSED(arg);

	app_svc_log("probe: calling the shared service library from an llext app");

	/* time group */
	say("uptime", app_svc_uptime_ms() > 0 ? 0 : -1);
	say("cycles", app_svc_cycles() > 0 ? 0 : -1);

	/* random group */
	uint8_t rnd[16] = {0};
	int rc = app_svc_random(rnd, sizeof(rnd));
	int nonzero = 0;

	for (int j = 0; j < 16; j++) {
		nonzero |= rnd[j];
	}
	say("random", (rc == 0 && nonzero) ? 0 : -1);

	/* crypto group */
	uint8_t digest[32] = {0};

	rc = app_svc_sha256((const uint8_t *)"hello", 5, digest);
	say("sha256", (rc == 0 && digest[0] != 0) ? 0 : -1);

	/* sensor group */
	say("die_temp", app_svc_die_temp_milli() > 0 ? 0 : -1);

	/* Typed sensor catalog: count, describe, read the present one. */
	int scount = app_svc_sensor_count();

	say("sensor_count", scount > 0 ? 0 : -1);

	struct app_svc_sensor_info sinfo;
	int sirc = app_svc_sensor_info(0, &sinfo);

	say("sensor_info", sirc == 0 ? 0 : sirc);
	say("sensor_info_range", app_svc_sensor_info(9999, &sinfo) == -EINVAL ? 0 : -1);

	int32_t sval = 0;
	int srrc = app_svc_sensor_read(0, &sval);

	say("sensor_read", srrc == 0 ? 0 : srrc);
	if (srrc == 0) {
		say("sensor_plausible",
		    (sval > -40000 && sval < 125000) ? 0 : -1);
	}

	/* kv group: round-trip a value through the lock-serialized store */
	say("kv_set", app_svc_kv_set("count", 7));
	uint32_t got = 0;

	rc = app_svc_kv_get("count", &got);
	say("kv_get", (rc == 0 && got == 7) ? 0 : -1);

	/* flash group: erase, write, read back the storage partition */
	uint8_t fbuf[16] = {0};

	say("flash_read", app_svc_flash_read(0, fbuf, sizeof(fbuf)));
	say("flash_erase", app_svc_flash_erase(0, 4096));
	const uint8_t fw[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

	app_svc_flash_write(0, fw, 4);
	uint8_t fr[4] = {0};

	app_svc_flash_read(0, fr, 4);
	say("flash_wr", (fr[0] == 0xDE && fr[3] == 0xEF) ? 0 : -1);

	/* hwinfo: device id and reset cause */
	uint8_t devid[12] = {0};

	say("device_id", app_svc_device_id(devid, sizeof(devid)) > 0 ? 0 : -1);
	uint32_t cause = 0;

	say("reset_cause", app_svc_reset_cause(&cause) == 0 ? 0 : -1);

	/* timer: arm 100ms and wait for it */
	app_svc_timer_start(100);
	say("timer", app_svc_timer_wait(500) == 0 ? 0 : -1);

	/* rtc: seed the wall clock and read it back */
	app_svc_rtc_set(1000000);
	uint32_t epoch = 0;

	app_svc_rtc_now(&epoch);
	say("rtc", epoch >= 1000000 ? 0 : -1);

	/* gpio group: line 0 is granted, line 1 is not. An ungranted line is
	 * -EACCES: the capability is held, the line is not assigned.
	 */
	say("gpio_count", app_svc_gpio_count() >= 2 ? 0 : -1);
	say("gpio_granted", app_svc_gpio_set(0, 1));
	say("gpio_denied", app_svc_gpio_set(1, 1) == -EACCES ? 0 : -1);

	/* ipc group: send to our own mailbox and read it back */
	const char *m = "ping";

	rc = app_svc_ipc_send(0, m, 5);
	uint8_t inbox[16] = {0};
	int got_len = app_svc_ipc_recv(inbox, sizeof(inbox), 100);

	say("ipc_send", rc);
	say("ipc_recv", (got_len == 5 && inbox[0] == 'p') ? 0 : -1);

	/* named addressing: register a name, send to ourselves by name */
	say("ipc_register", app_svc_ipc_register("probe"));
	app_svc_ipc_send_to("probe", "byname", 7);
	uint8_t nbx[16] = {0};

	say("ipc_byname", (app_svc_ipc_recv(nbx, sizeof(nbx), 100) == 7 &&
			   nbx[0] == 'b') ? 0 : -1);
	say("ipc_unknown", app_svc_ipc_send_to("nope", "x", 1) == -2 ? 0 : -1);

	/* pub/sub: subscribe to a topic, publish to it, receive the broadcast */
	app_svc_topic_subscribe("sensors/temp");
	int subs = app_svc_topic_publish("sensors/temp", "22.5", 5);
	uint8_t tbx[16] = {0};

	say("topic_pub", subs >= 1 ? 0 : -1);
	say("topic_recv", (app_svc_ipc_recv(tbx, sizeof(tbx), 100) == 5 &&
			   tbx[0] == '2') ? 0 : -1);

	/* can group: loopback send then receive the same frame back */
	const uint8_t cd[3] = { 0x11, 0x22, 0x33 };

	say("can_send", app_svc_can_send(0x123, cd, 3));
	uint32_t cid = 0;
	uint8_t crx[8] = {0};
	int cn = app_svc_can_recv(&cid, crx, sizeof(crx), 500);

	say("can_loop", (cn == 3 && cid == 0x123 && crx[0] == 0x11) ? 0 : -1);

	/* dma group: copy a buffer through the DMA engine and check it */
	const uint8_t ds[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t dd[8] = {0};

	say("dma_copy", (app_svc_dma_copy(ds, dd, 8) == 0 && dd[7] == 8) ? 0 : -1);

	/* inspect group: read our own group's status */
	struct app_svc_app_info info;

	rc = app_svc_app_info(0, &info);
	say("app_count", app_svc_app_count() > 0 ? 0 : -1);
	say("app_info", (rc == 0 && info.active) ? 0 : -1);

	/* periph group: PWM channel 0 and ADC channel 0, both granted */
	say("pwm_set", app_svc_pwm_set(0, 500));
	say("adc_read", app_svc_adc_read(0) >= 0 ? 0 : -1);

	/*
	 * bus group: with no device wired we check the capability and grant
	 * path. A granted I2C address reaches the driver (not -EPERM); an
	 * ungranted address is refused. UART is granted by capability alone.
	 */
	uint8_t b[2] = {0};

	say("i2c_granted", app_svc_i2c_read(0x50, b, 1) != -1 ? 0 : -1);
	say("i2c_denied", app_svc_i2c_read(0x51, b, 1) == -EACCES ? 0 : -1);
	/* Writing reaches the granted address and is refused elsewhere. A bus
	 * with nothing attached answers -EIO or -ETIMEDOUT, which still proves
	 * the path.
	 */
	const uint8_t reg = 0x00;
	int wr = app_svc_i2c_write(0x50, &reg, 1);

	say("i2c_write", (wr == 0 || wr == -EIO || wr == -ETIMEDOUT) ? 0 : wr);
	say("i2c_write_denied",
	    app_svc_i2c_write(0x51, &reg, 1) == -EACCES ? 0 : -1);
	int wrrd = app_svc_i2c_write_read(0x50, &reg, 1, b, 1);

	say("i2c_write_read",
	    (wrrd == 0 || wrrd == -EIO || wrrd == -ETIMEDOUT) ? 0 : wrrd);
	say("spi_xfer", app_svc_spi_xfer((const uint8_t *)"\x9f", b, 1) != -1 ? 0 : -1);
	/* uart: capability passes; no device wired returns a clean error, not a fault */
	say("uart_path", app_svc_uart_write((const uint8_t *)"hi", 2) != -1 ? 0 : -1);
	uint8_t urx[4] = {0};

	say("uart_read", app_svc_uart_read(urx, sizeof(urx)) != -1 ? 0 : -1);

#ifndef PROBE_NO_BLE
	/* ble scan: see a peer board advertising the heart-rate service */
	app_svc_log("ble: scanning for HR peer (up to 4s)...");
	say("ble_scan", app_svc_ble_scan("HR-Peer", 4000) == 1 ? 0 : -1);

	/* ble gatt: connect to the HR peer and read one measurement */
	app_svc_log("ble: connecting + reading heart rate (up to 8s)...");
	uint8_t bpm = 0;
	int hr = app_svc_ble_hr_read(8000, &bpm);

	say("ble_hr", (hr == 0 && bpm > 0) ? 0 : -1);
#endif

	/*
	 * radio group: send to the 802.15.4 peer and read back its echo. BLE and
	 * 802.15.4 share one radio on this SoC and cannot run at once, so this
	 * runs only in the radio-only build (BLE compiled out).
	 */
#if defined(PROBE_NO_BLE) && !defined(PROBE_NO_RADIO)
	app_svc_log("radio: sending to 802.15.4 peer, awaiting echo...");
	uint8_t rx[16] = {0};

	say("radio_send", app_svc_radio_send("hello-15.4", 10));
	int rn = app_svc_radio_recv(rx, sizeof(rx), 3000);

	say("radio_echo", (rn == 10 && rx[0] == 'h') ? 0 : -1);
#endif

	app_svc_log("probe: done - all from platform-owned services");
	for (;;) {
		app_svc_heartbeat();
		app_sleep_ms(1000);
	}
}
EXPORT_SYMBOL(app_main);
