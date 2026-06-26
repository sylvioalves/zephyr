/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A downloadable platform app, built as an llext and loaded at runtime. It
 * reaches the platform only through a service table the platform injects at
 * launch - the loadable-app equivalent of the service syscalls a built-in app
 * uses. The platform verifies the module, isolates it in its own memory domain,
 * grants it a capability set, and supervises it like any other app.
 */

#include "app_api.h"

#include <zephyr/llext/symbol.h>

/*
 * The module calls only through the injected service table - no kernel syscalls
 * directly - so it links cleanly as an llext (syscall stubs are not resolvable
 * from an extension on RISC-V).
 */
void app_main(const struct app_api *api)
{
	int on = 0;

	api->log("installed-llext: hello from a runtime-loaded app");

	for (int i = 0; i < 8; i++) {
		int rc = api->led_set(on);

		on = !on;
		if (i == 0 && rc != 0) {
			api->log("installed-llext: led denied (no CAP_GPIO)");
		}
		api->log("installed-llext: blink + heartbeat");
		api->heartbeat();
		api->sleep_ms(500);
	}

	api->log("installed-llext: done");
}
EXPORT_SYMBOL(app_main);
