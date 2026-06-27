/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The brokered MQTT service. One Wi-Fi interface, one network stack, one DNS
 * resolution of the broker - all owned by the supervisor and shared. Each app
 * gets its own mqtt_client session keyed by its group id, so two isolated apps
 * publish under their own broker identity and topic without carrying any of the
 * network stack themselves. A single supervisor net thread runs all sessions'
 * socket I/O; apps never touch a socket.
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
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

#define BROKER_HOST "test.mosquitto.org"
#define BROKER_PORT 1883
#define MAX_SESSIONS CONFIG_APP_PLATFORM_MAX_APPS
#define ID_MAX       24
#define TOPIC_MAX    48
#define PAYLOAD_MAX  64

/* One MQTT session per app, all sharing the single net stack below. */
struct session {
	bool                 used;
	bool                 connected;
	char                 client_id[ID_MAX];
	char                 topic[TOPIC_MAX];
	struct mqtt_client   client;
	struct sockaddr_storage broker;
	uint8_t              rx_buf[256];
	uint8_t              tx_buf[256];
};

static struct session sessions[MAX_SESSIONS];

/* App publish requests handed to the supervisor net thread. */
struct pub_req {
	int  id;
	char payload[PAYLOAD_MAX];
};
K_MSGQ_DEFINE(pub_q, sizeof(struct pub_req), 8, 4);

static K_SEM_DEFINE(net_ready, 0, 1);
static K_MUTEX_DEFINE(open_lock);
static struct net_mgmt_event_callback ipv4_cb;

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

	printk("net: connecting to '%s' (one stack for all apps)...\n",
	       CONFIG_APP_WIFI_SSID);
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
}

static struct sockaddr_storage broker_addr;
static bool broker_resolved;

static int resolve_broker(void)
{
	struct zsock_addrinfo *res, hints = { .ai_family = AF_INET,
					      .ai_socktype = SOCK_STREAM };
	int rc = zsock_getaddrinfo(BROKER_HOST, "1883", &hints, &res);

	if (rc != 0 || !res) {
		printk("net: DNS resolve failed: %d\n", rc);
		return -EIO;
	}
	struct sockaddr_in *b = (struct sockaddr_in *)&broker_addr;
	struct sockaddr_in *r = (struct sockaddr_in *)res->ai_addr;

	b->sin_family = AF_INET;
	b->sin_port = htons(BROKER_PORT);
	b->sin_addr = r->sin_addr;
	zsock_freeaddrinfo(res);
	broker_resolved = true;
	return 0;
}

static void mqtt_evt(struct mqtt_client *c, const struct mqtt_evt *e)
{
	struct session *s = CONTAINER_OF(c, struct session, client);

	if (e->type == MQTT_EVT_CONNACK) {
		s->connected = (e->result == 0);
		printk("mqtt: session '%s' %s\n", s->client_id,
		       s->connected ? "CONNECTED" : "refused");
	} else if (e->type == MQTT_EVT_DISCONNECT) {
		s->connected = false;
	}
}

static int session_connect(struct session *s)
{
	if (!broker_resolved && resolve_broker() != 0) {
		return -EIO;
	}
	memcpy(&s->broker, &broker_addr, sizeof(broker_addr));

	mqtt_client_init(&s->client);
	s->client.broker = &s->broker;
	s->client.evt_cb = mqtt_evt;
	s->client.client_id.utf8 = s->client_id;
	s->client.client_id.size = strlen(s->client_id);
	s->client.protocol_version = MQTT_VERSION_3_1_1;
	s->client.rx_buf = s->rx_buf; s->client.rx_buf_size = sizeof(s->rx_buf);
	s->client.tx_buf = s->tx_buf; s->client.tx_buf_size = sizeof(s->tx_buf);
	s->client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	if (mqtt_connect(&s->client) != 0) {
		printk("mqtt: session '%s' connect failed\n", s->client_id);
		return -EIO;
	}

	struct zsock_pollfd fd = { .fd = s->client.transport.tcp.sock,
				   .events = ZSOCK_POLLIN };

	for (int i = 0; i < 50 && !s->connected; i++) {
		zsock_poll(&fd, 1, 100);
		mqtt_input(&s->client);
	}
	return s->connected ? 0 : -ETIMEDOUT;
}

static void session_publish(struct session *s, const char *payload)
{
	struct mqtt_publish_param p = { 0 };

	p.message.topic.topic.utf8 = s->topic;
	p.message.topic.topic.size = strlen(s->topic);
	p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	p.message.payload.data = (uint8_t *)payload;
	p.message.payload.len = strlen(payload);
	p.message_id = sys_rand16_get();

	int rc = mqtt_publish(&s->client, &p);

	printk("mqtt: '%s' published '%s' to %s rc=%d\n",
	       s->client_id, payload, s->topic, rc);
}

/* The one supervisor thread that drives every session's socket. */
static void net_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		struct pub_req req;

		if (k_msgq_get(&pub_q, &req, K_MSEC(500)) == 0) {
			if (req.id >= 0 && req.id < MAX_SESSIONS &&
			    sessions[req.id].connected) {
				session_publish(&sessions[req.id], req.payload);
			}
		}

		for (int i = 0; i < MAX_SESSIONS; i++) {
			struct session *s = &sessions[i];

			if (s->connected) {
				struct zsock_pollfd fd = {
					.fd = s->client.transport.tcp.sock,
					.events = ZSOCK_POLLIN };

				zsock_poll(&fd, 1, 0);
				mqtt_input(&s->client);
				mqtt_live(&s->client);
			}
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

	/* Resolve the broker once, before any app opens a session. */
	for (int i = 0; i < 5 && resolve_broker() != 0; i++) {
		k_sleep(K_SECONDS(1));
	}
	if (!broker_resolved) {
		printk("net: broker DNS failed\n");
		return -EIO;
	}
	return 0;
}

/* ---- brokered service calls (supervisor side) ---- */

int z_impl_svc_mqtt_open(const char *client_id, const char *topic)
{
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS) {
		return -EINVAL;
	}

	struct session *s = &sessions[id];

	s->used = true;
	strncpy(s->client_id, client_id, sizeof(s->client_id) - 1);
	strncpy(s->topic, topic, sizeof(s->topic) - 1);

	/* Serialize concurrent opens; the connect/poll path is not reentrant. */
	k_mutex_lock(&open_lock, K_FOREVER);
	int rc = session_connect(s);

	k_mutex_unlock(&open_lock);
	return rc;
}

int z_impl_svc_mqtt_publish(const char *payload)
{
	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS || !sessions[id].connected) {
		return -EINVAL;
	}

	struct pub_req req = { .id = id };

	strncpy(req.payload, payload, sizeof(req.payload) - 1);
	return k_msgq_put(&pub_q, &req, K_NO_WAIT);
}

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
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

static inline int z_vrfy_svc_mqtt_open(const char *client_id, const char *topic)
{
	char cid[ID_MAX], top[TOPIC_MAX];

	if (k_usermode_string_copy(cid, (char *)client_id, sizeof(cid)) != 0 ||
	    k_usermode_string_copy(top, (char *)topic, sizeof(top)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_mqtt_open(cid, top);
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
#endif
