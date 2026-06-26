/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Real MQTT over Wi-Fi: the platform connects to Wi-Fi, resolves a public
 * Mosquitto broker, connects, subscribes to a command topic, and publishes the
 * die temperature every five seconds. This is the brokered MQTT service path
 * made real - the supervisor owns the client and the network; an app would call
 * publish/poll through it.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/random/random.h>
#include <string.h>

#define BROKER_HOST "test.mosquitto.org"
#define BROKER_PORT 1883
#define PUB_TOPIC   "zephyr/esp32c6/temp"
#define SUB_TOPIC   "zephyr/esp32c6/cmd"

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buf[256], tx_buf[256];
static struct zsock_pollfd fds[1];
static bool mqtt_connected;

static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));
static K_SEM_DEFINE(net_ready, 0, 1);

/* ---- Wi-Fi bring-up ---- */
static struct net_mgmt_event_callback ipv4_cb;

static void ipv4_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
		     struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (ev == NET_EVENT_IPV4_ADDR_ADD) {
		char ip[NET_IPV4_ADDR_LEN], gw[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET,
			&iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,
			ip, sizeof(ip));
		net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw, gw, sizeof(gw));
		printk("net: IP=%s GW=%s - ready\n", ip, gw);
		k_sem_give(&net_ready);
	}
}

static int wifi_up(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		return -ENODEV;
	}
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_evt, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	static struct wifi_connect_req_params p;

	p.ssid = CONFIG_APP_WIFI_SSID; p.ssid_length = strlen(CONFIG_APP_WIFI_SSID);
	p.psk = CONFIG_APP_WIFI_PSK;   p.psk_length = strlen(CONFIG_APP_WIFI_PSK);
	p.security = WIFI_SECURITY_TYPE_PSK; p.band = WIFI_FREQ_BAND_2_4_GHZ;
	p.channel = WIFI_CHANNEL_ANY; p.mfp = WIFI_MFP_OPTIONAL;

	printk("net: connecting to '%s'...\n", CONFIG_APP_WIFI_SSID);
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
}

/* ---- MQTT ---- */
static void mqtt_evt(struct mqtt_client *c, const struct mqtt_evt *e)
{
	switch (e->type) {
	case MQTT_EVT_CONNACK:
		mqtt_connected = (e->result == 0);
		printk("mqtt: CONNACK result=%d -> %s\n", e->result,
		       mqtt_connected ? "CONNECTED" : "refused");
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &e->param.publish;
		uint8_t buf[64];
		int len = MIN(p->message.payload.len, sizeof(buf) - 1);

		mqtt_read_publish_payload(c, buf, len);
		buf[len] = '\0';
		printk("mqtt: RX on subscribed topic: '%s'\n", buf);
		break;
	}
	case MQTT_EVT_DISCONNECT:
		mqtt_connected = false;
		printk("mqtt: disconnected\n");
		break;
	default:
		break;
	}
}

static int resolve_broker(void)
{
	struct zsock_addrinfo *res, hints = { .ai_family = AF_INET,
					      .ai_socktype = SOCK_STREAM };
	int rc = zsock_getaddrinfo(BROKER_HOST, "1883", &hints, &res);

	if (rc != 0 || !res) {
		printk("net: DNS resolve of %s failed: %d\n", BROKER_HOST, rc);
		return -EIO;
	}
	struct sockaddr_in *b = (struct sockaddr_in *)&broker;
	struct sockaddr_in *r = (struct sockaddr_in *)res->ai_addr;
	uint8_t *ip = (uint8_t *)&r->sin_addr;

	b->sin_family = AF_INET;
	b->sin_port = htons(BROKER_PORT);
	b->sin_addr = r->sin_addr;
	printk("net: %s -> %u.%u.%u.%u\n", BROKER_HOST, ip[0], ip[1], ip[2], ip[3]);
	zsock_freeaddrinfo(res);
	return 0;
}

static void client_init(void)
{
	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_evt;
	client.client_id.utf8 = "zephyr-c6"; client.client_id.size = 9;
	client.password = NULL; client.user_name = NULL;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf; client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf; client.tx_buf_size = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
}

static void subscribe(void)
{
	struct mqtt_topic t = { .topic = { .utf8 = SUB_TOPIC,
					   .size = strlen(SUB_TOPIC) },
				.qos = MQTT_QOS_0_AT_MOST_ONCE };
	struct mqtt_subscription_list s = { .list = &t, .list_count = 1,
					    .message_id = 1 };
	int rc = mqtt_subscribe(&client, &s);

	printk("mqtt: %s '%s' rc=%d\n", __func__, SUB_TOPIC, rc);
}

static void publish_temp(void)
{
	int32_t milli = 0;

	if (device_is_ready(die_temp) && sensor_sample_fetch(die_temp) == 0) {
		struct sensor_value v;

		sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v);
		milli = v.val1 * 1000 + v.val2 / 1000;
	}
	char payload[16];
	int n = snprintk(payload, sizeof(payload), "%d.%d",
			 milli / 1000, (milli % 1000) / 100);

	struct mqtt_publish_param p = { 0 };

	p.message.topic.topic.utf8 = PUB_TOPIC;
	p.message.topic.topic.size = strlen(PUB_TOPIC);
	p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	p.message.payload.data = payload;
	p.message.payload.len = n;
	p.message_id = sys_rand16_get();
	int rc = mqtt_publish(&client, &p);

	printk("mqtt: published '%s' to %s rc=%d\n", payload, PUB_TOPIC, rc);
}

int main(void)
{
	printk("\n*** real MQTT: publish die-temp to %s (C6) ***\n", BROKER_HOST);

	if (wifi_up() != 0 || k_sem_take(&net_ready, K_SECONDS(30)) != 0) {
		printk("net: Wi-Fi/IP not ready\n");
		return 0;
	}
	if (resolve_broker() != 0) {
		return 0;
	}

	client_init();
	if (mqtt_connect(&client) != 0) {
		printk("mqtt: connect failed\n");
		return 0;
	}

	fds[0].fd = client.transport.tcp.sock;
	fds[0].events = ZSOCK_POLLIN;

	/* Wait for CONNACK. */
	for (int i = 0; i < 50 && !mqtt_connected; i++) {
		zsock_poll(fds, 1, 100);
		mqtt_input(&client);
	}
	if (!mqtt_connected) {
		printk("mqtt: no CONNACK\n");
		return 0;
	}
	subscribe();

	/* Publish die-temp every 5s; keep the connection alive. */
	int64_t last = 0;

	for (;;) {
		zsock_poll(fds, 1, 1000);
		mqtt_input(&client);
		mqtt_live(&client);
		if (k_uptime_get() - last >= 5000) {
			last = k_uptime_get();
			publish_temp();
		}
	}
	return 0;
}
