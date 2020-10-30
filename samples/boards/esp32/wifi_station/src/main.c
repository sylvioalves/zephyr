/*
 * Copyright (c) 2020 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/printk.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_event.h>

#include <net/net_if.h>
#include <net/net_core.h>
#include <net/net_context.h>
#include <net/net_mgmt.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(esp32_wifi_sta, LOG_LEVEL_DBG);

static struct net_mgmt_event_callback mgmt_cb;

static void handler(struct net_mgmt_event_callback *cb,
		    uint32_t mgmt_event,
		    struct net_if *iface)
{
	int i = 0;

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		char buf[NET_IPV4_ADDR_LEN];

		if (iface->config.ip.ipv4->unicast[i].addr_type !=
		    NET_ADDR_DHCP) {
			continue;
		}

		LOG_INF("Your address: %s", log_strdup(net_addr_ntop(AF_INET,
							&iface->config.ip.ipv4->unicast[i].address.in_addr,
							buf, sizeof(buf))));

		LOG_INF("Lease time: %u seconds", iface->config.dhcpv4.lease_time);

		LOG_INF("Subnet: %s", log_strdup(net_addr_ntop(AF_INET,
							&iface->config.ip.ipv4->netmask, buf,
							sizeof(buf))));

		LOG_INF("Router: %s", log_strdup(net_addr_ntop(AF_INET,
							&iface->config.ip.ipv4->gw, buf, sizeof(buf))));
	}
}

void main(void)
{
	struct net_if *iface;

	net_mgmt_init_event_callback(&mgmt_cb, handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&mgmt_cb);

	iface = net_if_get_default();

	net_dhcpv4_start(iface);

	if (!IS_ENABLED(CONFIG_WIFI_STA_AUTO)) {
		wifi_config_t wifi_config = {
			.sta = {
				.ssid = CONFIG_WIFI_SSID,
				.password = CONFIG_WIFI_PASSWORD,
			},
		};

		esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
		ret |= esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
		ret |= esp_wifi_connect();
		if (ret != ESP_OK) {
			LOG_ERR("connection failed");
		}
	}
}
