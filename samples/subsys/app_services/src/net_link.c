/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "net_link.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>

/* Counting sem: one permit per waiting app, topped up when the link is ready. */
K_SEM_DEFINE(net_ready_sem, 0, 8);

uint8_t net_link_gateway[4];

static atomic_t link_ready;

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

#define WIFI_MGMT_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
			  NET_EVENT_WIFI_DISCONNECT_RESULT)

static void wifi_evt(struct net_mgmt_event_callback *cb,
		     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		/* The driver auto-starts DHCP (WIFI_STA_AUTO_DHCPV4), which also
		 * installs the default route. We just wait for the IPv4 event.
		 */
		printk("net_link: Wi-Fi connected, waiting for DHCP/IP\n");
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		printk("net_link: Wi-Fi disconnected\n");
		break;
	default:
		break;
	}
}

static void ipv4_evt(struct net_mgmt_event_callback *cb,
		     uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		/* Diagnostic: show the assigned IP and gateway. */
		if (iface != NULL && iface->config.ip.ipv4 != NULL) {
			char ip[NET_IPV4_ADDR_LEN], gw[NET_IPV4_ADDR_LEN];

			net_addr_ntop(AF_INET,
				&iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
				ip, sizeof(ip));
			net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw,
				      gw, sizeof(gw));
			memcpy(net_link_gateway, &iface->config.ip.ipv4->gw, 4);
			printk("net_link: IP=%s GW=%s\n", ip, gw);
		}
		printk("net_link: got IPv4 address - network READY\n");
		atomic_set(&link_ready, 1);
		/* Release every app that is (or will be) waiting. */
		for (int i = 0; i < 8; i++) {
			k_sem_give(&net_ready_sem);
		}
	}
}

int net_link_up(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (iface == NULL) {
		printk("net_link: no Wi-Fi iface\n");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&wifi_cb, wifi_evt, WIFI_MGMT_EVENTS);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_evt, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	static struct wifi_connect_req_params params;

	params.ssid = (const uint8_t *)CONFIG_APP_WIFI_SSID;
	params.ssid_length = strlen(CONFIG_APP_WIFI_SSID);
	params.psk = (const uint8_t *)CONFIG_APP_WIFI_PSK;
	params.psk_length = strlen(CONFIG_APP_WIFI_PSK);
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.channel = WIFI_CHANNEL_ANY;
	params.mfp = WIFI_MFP_OPTIONAL;

	printk("net_link: connecting to '%s'...\n", CONFIG_APP_WIFI_SSID);
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
			  sizeof(params));
	if (rc != 0) {
		printk("net_link: connect request failed: %d\n", rc);
		return rc;
	}
	return 0;
}

int net_link_wait_ready(k_timeout_t timeout)
{
	return k_sem_take(&net_ready_sem, timeout);
}
