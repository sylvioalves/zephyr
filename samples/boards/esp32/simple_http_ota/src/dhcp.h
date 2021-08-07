/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DHCP_H__
#define __DHCP_H__

#define DHCP_TIMEOUT K_SECONDS(30)

/**
 * @brief Init DHCPv4 interface
 *
 * Start DHCP interface and wait DHCP_TIMEOUT seconds to
 * get IP address assignement.
 *
 * @return N/A
 */
void app_dhcpv4_startup(void);

#endif
