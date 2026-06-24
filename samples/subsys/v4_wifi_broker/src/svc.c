/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/internal/syscall_handler.h>

int z_impl_svc_wifi_scan(struct k_sem *token)
{
	ARG_UNUSED(token);

	struct net_if *iface = net_if_get_first_wifi();

	if (iface == NULL) {
		iface = net_if_get_default();
	}
	if (iface == NULL) {
		return -ENODEV;
	}

	/* Runs in supervisor mode: drive the radio via net_mgmt. */
	return net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_wifi_scan(struct k_sem *token)
{
	K_OOPS(K_SYSCALL_OBJ(token, K_OBJ_SEM));
	return z_impl_svc_wifi_scan(token);
}
#include <zephyr/syscalls/svc_wifi_scan_mrsh.c>
#endif /* CONFIG_USERSPACE */
