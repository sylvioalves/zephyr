/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The supervisor-side MQTT service. It owns the Wi-Fi interface, the network
 * stack, the socket, the MQTT client and the die-temperature sensor. A loadable
 * app opens a session, subscribes to a command topic and publishes through the
 * brokered svc_ calls below; a supervisor net thread runs the socket I/O and
 * prints anything received on the subscribed topic. The app holds none of this.
 */

#include "svc.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

#define BROKER_HOST "test.mosquitto.org"
#define BROKER_PORT 1883
#define ID_MAX      24
#define TOPIC_MAX   48
#define PAYLOAD_MAX 64

static const struct device *const die_temp = DEVICE_DT_GET(DT_ALIAS(die_temp0));

static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buf[256], tx_buf[256];
static bool mqtt_connected;
static char pub_topic[TOPIC_MAX];
static char sub_topic[TOPIC_MAX];

static K_SEM_DEFINE(net_ready, 0, 1);
static K_MUTEX_DEFINE(mqtt_lock);
static struct net_mgmt_event_callback ipv4_cb;

struct pub_req {
	char payload[PAYLOAD_MAX];
};
K_MSGQ_DEFINE(pub_q, sizeof(struct pub_req), 8, 4);

static void ipv4_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
		     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (ev == NET_EVENT_IPV4_ADDR_ADD) {
		printk("net: IP acquired - shared stack ready\n");
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

static int resolve_broker(void)
{
	struct zsock_addrinfo *res, hints = { .ai_family = AF_INET,
					      .ai_socktype = SOCK_STREAM };
	int rc = zsock_getaddrinfo(BROKER_HOST, "1883", &hints, &res);

	if (rc != 0 || !res) {
		return -EIO;
	}
	struct sockaddr_in *b = (struct sockaddr_in *)&broker;
	struct sockaddr_in *r = (struct sockaddr_in *)res->ai_addr;

	b->sin_family = AF_INET;
	b->sin_port = htons(BROKER_PORT);
	b->sin_addr = r->sin_addr;
	zsock_freeaddrinfo(res);
	return 0;
}

static void mqtt_evt(struct mqtt_client *c, const struct mqtt_evt *e)
{
	switch (e->type) {
	case MQTT_EVT_CONNACK:
		mqtt_connected = (e->result == 0);
		printk("mqtt: %s\n", mqtt_connected ? "CONNECTED" : "refused");
		break;
	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &e->param.publish;
		uint8_t buf[64];
		int len = MIN(p->message.payload.len, sizeof(buf) - 1);

		mqtt_read_publish_payload(c, buf, len);
		buf[len] = '\0';
		printk("mqtt: RX on '%s': '%s'\n", sub_topic, buf);
		break;
	}
	case MQTT_EVT_DISCONNECT:
		mqtt_connected = false;
		break;
	default:
		break;
	}
}

static void subscribe(void)
{
	struct mqtt_topic t = { .topic = { .utf8 = sub_topic,
					   .size = strlen(sub_topic) },
				.qos = MQTT_QOS_0_AT_MOST_ONCE };
	struct mqtt_subscription_list s = { .list = &t, .list_count = 1,
					    .message_id = 1 };
	int rc = mqtt_subscribe(&client, &s);

	printk("mqtt: subscribed to '%s' rc=%d\n", sub_topic, rc);
}

static void net_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		struct pub_req req;

		if (k_msgq_get(&pub_q, &req, K_MSEC(500)) == 0 && mqtt_connected) {
			struct mqtt_publish_param p = { 0 };

			p.message.topic.topic.utf8 = pub_topic;
			p.message.topic.topic.size = strlen(pub_topic);
			p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
			p.message.payload.data = (uint8_t *)req.payload;
			p.message.payload.len = strlen(req.payload);
			p.message_id = sys_rand16_get();

			k_mutex_lock(&mqtt_lock, K_FOREVER);
			int rc = mqtt_publish(&client, &p);

			k_mutex_unlock(&mqtt_lock);
			printk("mqtt: published '%s' to %s rc=%d\n",
			       req.payload, pub_topic, rc);
		}

		if (mqtt_connected) {
			struct zsock_pollfd fd = {
				.fd = client.transport.tcp.sock,
				.events = ZSOCK_POLLIN };

			zsock_poll(&fd, 1, 0);
			k_mutex_lock(&mqtt_lock, K_FOREVER);
			mqtt_input(&client);
			mqtt_live(&client);
			k_mutex_unlock(&mqtt_lock);
		}
	}
}

K_THREAD_DEFINE(net_tid, 4096, net_thread, NULL, NULL, NULL, 6, 0, 0);

int net_link_up(void)
{
	if (wifi_up() != 0 || k_sem_take(&net_ready, K_SECONDS(30)) != 0) {
		printk("net: Wi-Fi/IP not ready\n");
		return -EIO;
	}
	for (int i = 0; i < 5 && resolve_broker() != 0; i++) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}

/* ---- brokered services (supervisor side) ---- */

int z_impl_svc_mqtt_open(const char *client_id, const char *p_topic,
			 const char *s_topic)
{
	strncpy(pub_topic, p_topic, sizeof(pub_topic) - 1);
	strncpy(sub_topic, s_topic, sizeof(sub_topic) - 1);

	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_evt;
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = strlen(client_id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf; client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf; client.tx_buf_size = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	if (mqtt_connect(&client) != 0) {
		return -EIO;
	}

	struct zsock_pollfd fd = { .fd = client.transport.tcp.sock,
				   .events = ZSOCK_POLLIN };

	for (int i = 0; i < 50 && !mqtt_connected; i++) {
		zsock_poll(&fd, 1, 100);
		mqtt_input(&client);
	}
	if (!mqtt_connected) {
		return -ETIMEDOUT;
	}
	subscribe();
	return 0;
}

int z_impl_svc_mqtt_publish(const char *payload)
{
	if (!mqtt_connected) {
		return -EIO;
	}
	struct pub_req req;

	strncpy(req.payload, payload, sizeof(req.payload) - 1);
	req.payload[sizeof(req.payload) - 1] = '\0';
	return k_msgq_put(&pub_q, &req, K_NO_WAIT);
}

int z_impl_svc_die_temp_milli(void)
{
	if (!device_is_ready(die_temp) || sensor_sample_fetch(die_temp) != 0) {
		return 0;
	}
	struct sensor_value v;

	sensor_channel_get(die_temp, SENSOR_CHAN_DIE_TEMP, &v);
	return v.val1 * 1000 + v.val2 / 1000;
}

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

void z_impl_svc_heartbeat(void)
{
	app_platform_heartbeat();
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_svc_log(const char *msg)
{
	char buf[64];

	if (k_usermode_string_copy(buf, (char *)msg, sizeof(buf)) == 0) {
		z_impl_svc_log(buf);
	}
}
#include <zephyr/syscalls/svc_log_mrsh.c>

static inline void z_vrfy_svc_heartbeat(void)
{
	z_impl_svc_heartbeat();
}
#include <zephyr/syscalls/svc_heartbeat_mrsh.c>

static inline int z_vrfy_svc_mqtt_open(const char *client_id,
				       const char *p_topic, const char *s_topic)
{
	char cid[ID_MAX], pt[TOPIC_MAX], st[TOPIC_MAX];

	if (k_usermode_string_copy(cid, (char *)client_id, sizeof(cid)) != 0 ||
	    k_usermode_string_copy(pt, (char *)p_topic, sizeof(pt)) != 0 ||
	    k_usermode_string_copy(st, (char *)s_topic, sizeof(st)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_mqtt_open(cid, pt, st);
}
#include <zephyr/syscalls/svc_mqtt_open_mrsh.c>

static inline int z_vrfy_svc_mqtt_publish(const char *payload)
{
	char buf[PAYLOAD_MAX];

	if (k_usermode_string_copy(buf, (char *)payload, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_mqtt_publish(buf);
}
#include <zephyr/syscalls/svc_mqtt_publish_mrsh.c>

static inline int z_vrfy_svc_die_temp_milli(void)
{
	return z_impl_svc_die_temp_milli();
}
#include <zephyr/syscalls/svc_die_temp_milli_mrsh.c>
#endif
