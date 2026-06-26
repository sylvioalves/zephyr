/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_IPC_APP_CAPS_H_
#define APP_IPC_APP_CAPS_H_

#include <zephyr/sys/util.h>

/* Application-defined capability: permission to use inter-app messaging. */
#define CAP_IPC  BIT(0)

/* The well-known group ids in this demo's launch order. */
#define APP_PRODUCER 0
#define APP_CONSUMER 1

#endif /* APP_IPC_APP_CAPS_H_ */
