/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLATFORM_APP_CAPS_H_
#define PLATFORM_APP_CAPS_H_

#include <zephyr/sys/util.h>

/*
 * Application-defined capabilities. The platform stores each app's grant mask
 * opaquely; these bits give it meaning, and the services enforce them. This is
 * policy owned by the application, not the platform.
 */
#define CAP_SENSOR  BIT(0)   /* read the die temperature */
#define CAP_GPIO    BIT(1)   /* drive the status LED     */
#define CAP_PWM     BIT(2)   /* set the PWM duty         */
#define CAP_FLASH   BIT(3)   /* read/write its flash slot */
#define CAP_RTC     BIT(4)   /* read the wall clock      */

/* Resolve a catalog entry by installed name (defined in apps.c). */
const struct app_def *platform_catalog_find(const char *name);

#endif /* PLATFORM_APP_CAPS_H_ */
