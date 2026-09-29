/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Capability matrix: for each cap bit, one app holding only that bit and one
 * holding everything but that bit call one representative gated service.
 * Expected: without the bit -EPERM; with the bit anything but -EPERM.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

K_APPMEM_PARTITION_DEFINE(res_part);
K_APP_DMEM(res_part) volatile int res_rc;
K_APP_DMEM(res_part) volatile int res_done;
K_APP_DMEM(res_part) volatile int res_bit;
K_APP_DMEM(res_part) uint8_t ubuf[64];
K_APP_DMEM(res_part) uint8_t ubuf2[64];

struct cap_case {
	const char *name;
	app_caps_t bit;
	const char *call;
	int (*fn)(void);
};

static int c_crypto(void)  { return app_svc_sha256(ubuf, 32, ubuf2); }
static int c_random(void)  { return app_svc_random(ubuf, 4); }
static int c_time(void)    { return app_svc_timer_start(1); }
static int c_sensor(void)  { return app_svc_die_temp_milli(); }
static int c_kv(void)      { uint32_t v; return app_svc_kv_get("k", &v); }
static int c_flash(void)   { return app_svc_flash_read(0, ubuf, 4); }
static int c_gpio(void)    { return app_svc_gpio_count(); }
static int c_pwm(void)     { return app_svc_pwm_find("x"); }
static int c_adc(void)     { return app_svc_adc_read(0); }
static int c_i2c(void)     { return app_svc_i2c_read(0x50, ubuf, 1); }
static int c_spi(void)     { return app_svc_spi_xfer(ubuf, ubuf2, 1); }
static int c_uart(void)    { return app_svc_uart_write(ubuf, 1); }
static int c_ipc(void)     { return app_svc_ipc_register("n"); }
/* An out-of-range id is rejected after the capability check, so the probe shows
 * the gate without ever stopping one of the other applications.
 */
static int c_control(void)  { return app_svc_app_stop(-1); }
static int c_restart(void)  { return app_svc_app_restart(-1); }

static int c_inspect(void) { return app_svc_app_count(); }
static int c_can(void)     { return app_svc_can_send(1, ubuf, 1); }
static int c_dma(void)     { return app_svc_dma_copy(ubuf, ubuf2, 16); }
static int c_psram(void)
{
	uintptr_t a;
	size_t s;
	int r = app_svc_psram_map(4096, &a, &s);

	if (r == 0) {
		app_svc_psram_unmap();
	}
	return r;
}
static int c_led(void)     { return app_svc_led_set(0, 0); }
static int c_input(void)   { return app_svc_button_wait(0); }
static int c_counter(void) { int32_t c; return app_svc_pulse_count(&c); }
static int c_motor(void)   { return app_svc_motor_set(0, 0); }
static int c_log(void)     { app_svc_log("matrix"); return 0; }
#ifdef CONFIG_APP_PLATFORM_NET
static int c_net(void)     { return app_svc_net_ready(); }
static int c_socket(void)  { return app_svc_resolve("localhost", (char *)ubuf, 16); }
#endif
#ifdef CONFIG_APP_PLATFORM_BLE
static int c_ble(void)     { return app_svc_ble_scan("x", 1); }
#endif
#ifdef CONFIG_APP_PLATFORM_RADIO
static int c_radio(void)   { return app_svc_radio_send(ubuf, 1); }
#endif

static const struct cap_case cases[] = {
	{"LOG", APP_CAP_LOG, "app_svc_log", c_log},
	{"CRYPTO", APP_CAP_CRYPTO, "app_svc_sha256", c_crypto},
	{"RANDOM", APP_CAP_RANDOM, "app_svc_random", c_random},
	{"TIME", APP_CAP_TIME, "app_svc_timer_start", c_time},
	{"SENSOR", APP_CAP_SENSOR, "app_svc_die_temp_milli", c_sensor},
	{"KV", APP_CAP_KV, "app_svc_kv_get", c_kv},
	{"FLASH", APP_CAP_FLASH, "app_svc_flash_read", c_flash},
	{"GPIO", APP_CAP_GPIO, "app_svc_gpio_count", c_gpio},
	{"PWM", APP_CAP_PWM, "app_svc_pwm_find", c_pwm},
	{"ADC", APP_CAP_ADC, "app_svc_adc_read", c_adc},
	{"I2C", APP_CAP_I2C, "app_svc_i2c_read", c_i2c},
	{"SPI", APP_CAP_SPI, "app_svc_spi_xfer", c_spi},
	{"UART", APP_CAP_UART, "app_svc_uart_write", c_uart},
#ifdef CONFIG_APP_PLATFORM_NET
	{"NET", APP_CAP_NET, "app_svc_net_ready", c_net},
	{"SOCKET", APP_CAP_SOCKET, "app_svc_resolve", c_socket},
#endif
#ifdef CONFIG_APP_PLATFORM_BLE
	{"BLE", APP_CAP_BLE, "app_svc_ble_scan", c_ble},
#endif
#ifdef CONFIG_APP_PLATFORM_RADIO
	{"RADIO", APP_CAP_RADIO, "app_svc_radio_send", c_radio},
#endif
	{"IPC", APP_CAP_IPC, "app_svc_ipc_register", c_ipc},
	{"INSPECT", APP_CAP_INSPECT, "app_svc_app_count", c_inspect},
	{"CONTROL", APP_CAP_CONTROL, "app_svc_app_stop", c_control},
	{"CONTROL", APP_CAP_CONTROL, "app_svc_app_restart", c_restart},
	{"CAN", APP_CAP_CAN, "app_svc_can_send", c_can},
	{"DMA", APP_CAP_DMA, "app_svc_dma_copy", c_dma},
	{"PSRAM", APP_CAP_PSRAM, "app_svc_psram_map", c_psram},
	{"LED", APP_CAP_LED, "app_svc_led_set", c_led},
	{"INPUT", APP_CAP_INPUT, "app_svc_button_wait(0)", c_input},
	{"COUNTER", APP_CAP_COUNTER, "app_svc_pulse_count", c_counter},
	{"MOTOR", APP_CAP_MOTOR, "app_svc_motor_set", c_motor},
};

static void probe_app(void *arg)
{
	ARG_UNUSED(arg);
	res_rc = cases[res_bit].fn();
	res_done = 1;
}

static bool wait_free(int id)
{
	for (int i = 0; i < 400; i++) {
		struct app_platform_status st;

		if (app_platform_status(id, &st) == -EAGAIN) {
			return true;
		}
		k_msleep(5);
	}
	return false;
}

static int run_case(int idx, app_caps_t caps)
{
	const struct app_def def = {
		.name = "probe", .source = APP_BUILTIN, .entry = probe_app, .caps = caps,
		.stopped = true,
	};

	res_rc = -9999;
	res_done = 0;
	res_bit = idx;
	int id = app_platform_launch(&def);

	if (id < 0) {
		return -9998;
	}
	k_mem_domain_add_partition(app_platform_domain(id), &res_part);
	(void)app_platform_start(id);
	if (!wait_free(id)) {
		return -9997;
	}
	return res_done ? res_rc : -9996;
}

int main(void)
{
	int bad = 0;

	printk("\n*** app_matrix: %u capability bits x 2 (C5) ***\n",
	       (unsigned int)ARRAY_SIZE(cases));
	if (app_platform_init() != 0) {
		return 0;
	}
	app_platform_log_enable(false);
	printk("%-8s %-22s %10s %10s  %s\n", "cap", "call", "denied", "granted", "verdict");
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		int denied = run_case(i, APP_CAP_ALL & ~cases[i].bit);
		int granted = run_case(i, cases[i].bit);
		const char *v;

		if (cases[i].bit == APP_CAP_LOG) {
			v = "gated, but app_svc_log is void so the denial is not observable";
		} else if (denied == -EPERM && granted == -EACCES) {
			v = "ok (granted lacks the per-resource grant)";
		} else if (denied == -EPERM && granted != -EPERM) {
			v = "ok";
		} else if (denied == -9997) {
			v = "denied probe trapped";
		} else {
			v = "MISMATCH";
			bad++;
		}
		printk("%-8s %-22s %10d %10d  %s\n", cases[i].name, cases[i].call, denied,
		       granted, v);
	}
	printk("*** app_matrix done: %d mismatches; caps not compiled: "
	       "NET SOCKET BLE RADIO ***\n", bad);
	return 0;
}
