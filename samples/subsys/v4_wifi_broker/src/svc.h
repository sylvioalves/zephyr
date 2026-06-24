/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef V4_SVC_H_
#define V4_SVC_H_

#include <zephyr/kernel.h>

/*
 * Brokered Wi-Fi service: a user app requests a scan via this syscall, gated on
 * the caller holding the service-token kobject. The actual net_mgmt(WIFI_SCAN)
 * runs in supervisor mode inside z_impl - the user app never touches the radio
 * or the net stack directly. Tests that Wi-Fi coexists with USERSPACE/PMP on C6.
 */
__syscall int svc_wifi_scan(struct k_sem *token);

#include <zephyr/syscalls/svc.h>

#endif /* V4_SVC_H_ */
