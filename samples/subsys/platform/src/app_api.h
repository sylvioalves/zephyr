/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLATFORM_APP_API_H_
#define PLATFORM_APP_API_H_

#include <stdint.h>
#include <stddef.h>

/*
 * The service table injected into a loadable (llext) app at launch. It is the
 * loadable-app equivalent of the service syscalls a built-in app calls: each
 * entry is brokered by the platform, which enforces the app's capabilities, so
 * a call to an ungranted service returns an error just as a syscall would.
 */
struct app_api {
	void (*log)(const char *msg);
	void (*heartbeat)(void);
	int  (*sensor_read)(int32_t *milli_c);
	int  (*led_set)(int on);
	int  (*pwm_set)(uint8_t percent);
	void (*sleep_ms)(uint32_t ms);
};

/* A loadable app entry takes the injected service table. */
typedef void (*app_api_entry_t)(const struct app_api *api);

#endif /* PLATFORM_APP_API_H_ */
