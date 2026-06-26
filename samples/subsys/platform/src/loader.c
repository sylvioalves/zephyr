/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The loadable-app path: verify a downloaded module by SHA-256, load it as an
 * llext, resolve its entry symbol, and hand it to the manager to run as an
 * isolated user-mode group with granted capabilities. The bytes are embedded
 * for the demo, but verify -> load -> isolate -> run is the real install path.
 */

#include "platform.h"
#include "svc.h"
#include "app_api.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/buf_loader.h>
#include <psa/crypto.h>
#include <string.h>

extern int manager_launch_llext(const char *name, struct llext *ext,
				app_api_entry_t entry, const struct app_api *api,
				uint32_t caps);

/*
 * The service table handed to a loadable app. Each wrapper calls the brokered
 * service, which checks the running app's capabilities - so a loadable app is
 * gated exactly like a built-in one, without needing user-mode syscalls (which
 * llext does not support on RISC-V).
 */
static void api_sleep_ms(uint32_t ms)
{
	k_msleep((int32_t)ms);
}

static const struct app_api app_api = {
	.log         = z_impl_svc_log,
	.heartbeat   = z_impl_svc_heartbeat,
	.sensor_read = z_impl_svc_sensor_read,
	.led_set     = z_impl_svc_led_set,
	.pwm_set     = z_impl_svc_pwm_set,
	.sleep_ms    = api_sleep_ms,
};

static uint8_t module_buf[] = {
#include "installed_app.inc"
};

static const uint8_t expected_hash[32] = {
#include "installed_app.sha256.inc"
};

static int sha256(const uint8_t *buf, size_t len, uint8_t out[32])
{
	size_t olen;
	psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, buf, len, out, 32, &olen);

	return (st == PSA_SUCCESS && olen == 32) ? 0 : -EIO;
}

int loader_install(const char *name, uint32_t caps)
{
	uint8_t got[32];
	size_t len = ARRAY_SIZE(module_buf);

	if (psa_crypto_init() != PSA_SUCCESS) {
		return -EIO;
	}

	printk("loader: installing '%s' (%zu bytes)\n", name, len);

	if (sha256(module_buf, len, got) != 0) {
		return -EIO;
	}
	if (memcmp(got, expected_hash, 32) != 0) {
		printk("loader: VERIFY FAILED - hash mismatch, install REJECTED\n");
		return -EPERM;
	}
	printk("loader: verify OK (sha-256 matches)\n");

	struct llext_buf_loader bl = LLEXT_BUF_LOADER(module_buf, len);
	struct llext_load_param parm = LLEXT_LOAD_PARAM_DEFAULT;
	struct llext *ext = NULL;
	int rc = llext_load(&bl.loader, name, &ext, &parm);

	if (rc != 0) {
		printk("loader: llext_load failed: %d\n", rc);
		return rc;
	}

	app_api_entry_t entry = llext_find_sym(&ext->exp_tab, "app_main");

	if (entry == NULL) {
		printk("loader: no app_main entry\n");
		llext_unload(&ext);
		return -ENOENT;
	}

	return manager_launch_llext(name, ext, entry, &app_api, caps);
}
