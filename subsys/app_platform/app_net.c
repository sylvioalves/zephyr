/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The shared networking and MQTT service. The Wi-Fi interface, the network
 * stack and the socket all live here in the platform; each application gets its
 * own MQTT session keyed by its group id, so several isolated applications
 * publish under their own identity over one shared stack. A supervisor net
 * thread runs every session's socket I/O. Applications hold no socket and link
 * none of this code.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <string.h>
#include <errno.h>

#define BROKER_HOST  CONFIG_APP_PLATFORM_NET_BROKER_HOST
#define BROKER_PORT  1883
#define MAX_SESSIONS CONFIG_APP_PLATFORM_MAX_APPS
#define ID_MAX       24
#define TOPIC_MAX    48
#define PAYLOAD_MAX  64

static inline bool app_has_cap(uint32_t mask)
{
	return (app_platform_caps_current() & mask) == mask;
}

struct session {
	bool                    used;
	bool                    connected;
	char                    client_id[ID_MAX];
	char                    pub_topic[TOPIC_MAX];
	char                    sub_topic[TOPIC_MAX];
	struct mqtt_client      client;
	struct sockaddr_storage broker;
	uint8_t                 rx_buf[256];
	uint8_t                 tx_buf[256];
};

static struct session sessions[MAX_SESSIONS];

struct pub_req {
	int  id;
	char payload[PAYLOAD_MAX];
};
K_MSGQ_DEFINE(net_pub_q, sizeof(struct pub_req), 8, 4);

static K_SEM_DEFINE(net_ready_sem, 0, 1);
static K_MUTEX_DEFINE(net_lock);
static struct net_mgmt_event_callback ipv4_cb;
static struct sockaddr_storage broker_addr;
static bool link_started;
static bool link_ready;
static bool broker_resolved;

static void ipv4_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
		     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (ev == NET_EVENT_IPV4_ADDR_ADD) {
		printk("net: IP acquired - shared stack ready\n");
		k_sem_give(&net_ready_sem);
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

	p.ssid = CONFIG_APP_PLATFORM_NET_WIFI_SSID;
	p.ssid_length = strlen(CONFIG_APP_PLATFORM_NET_WIFI_SSID);
	p.psk = CONFIG_APP_PLATFORM_NET_WIFI_PSK;
	p.psk_length = strlen(CONFIG_APP_PLATFORM_NET_WIFI_PSK);
	p.security = WIFI_SECURITY_TYPE_PSK;
	p.band = WIFI_FREQ_BAND_2_4_GHZ;
	p.channel = WIFI_CHANNEL_ANY;
	p.mfp = WIFI_MFP_OPTIONAL;

	printk("net: connecting to '%s'...\n", CONFIG_APP_PLATFORM_NET_WIFI_SSID);
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
	memcpy(&s->broker, &broker_addr, sizeof(broker_addr));

	mqtt_client_init(&s->client);
	s->client.broker = &s->broker;
	s->client.evt_cb = mqtt_evt;
	s->client.client_id.utf8 = (uint8_t *)s->client_id;
	s->client.client_id.size = strlen(s->client_id);
	s->client.protocol_version = MQTT_VERSION_3_1_1;
	s->client.rx_buf = s->rx_buf; s->client.rx_buf_size = sizeof(s->rx_buf);
	s->client.tx_buf = s->tx_buf; s->client.tx_buf_size = sizeof(s->tx_buf);
	s->client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	if (mqtt_connect(&s->client) != 0) {
		return -EIO;
	}

	struct zsock_pollfd fd = { .fd = s->client.transport.tcp.sock,
				   .events = ZSOCK_POLLIN };

	for (int i = 0; i < 50 && !s->connected; i++) {
		zsock_poll(&fd, 1, 100);
		mqtt_input(&s->client);
	}
	if (!s->connected) {
		return -ETIMEDOUT;
	}

	struct mqtt_topic t = { .topic = { .utf8 = s->sub_topic,
					   .size = strlen(s->sub_topic) },
				.qos = MQTT_QOS_0_AT_MOST_ONCE };
	struct mqtt_subscription_list sl = { .list = &t, .list_count = 1,
					     .message_id = 1 };

	mqtt_subscribe(&s->client, &sl);
	return 0;
}

static void net_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		struct pub_req req;

		if (k_msgq_get(&net_pub_q, &req, K_MSEC(500)) == 0 &&
		    req.id >= 0 && req.id < MAX_SESSIONS &&
		    sessions[req.id].connected) {
			struct session *s = &sessions[req.id];
			struct mqtt_publish_param p = { 0 };

			p.message.topic.topic.utf8 = s->pub_topic;
			p.message.topic.topic.size = strlen(s->pub_topic);
			p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
			p.message.payload.data = (uint8_t *)req.payload;
			p.message.payload.len = strlen(req.payload);

			k_mutex_lock(&net_lock, K_FOREVER);
			int rc = mqtt_publish(&s->client, &p);

			k_mutex_unlock(&net_lock);
			printk("mqtt: '%s' published '%s' rc=%d\n",
			       s->client_id, req.payload, rc);
		}

		for (int i = 0; i < MAX_SESSIONS; i++) {
			struct session *s = &sessions[i];

			if (s->connected) {
				struct zsock_pollfd fd = {
					.fd = s->client.transport.tcp.sock,
					.events = ZSOCK_POLLIN };

				zsock_poll(&fd, 1, 0);
				k_mutex_lock(&net_lock, K_FOREVER);
				mqtt_input(&s->client);
				mqtt_live(&s->client);
				k_mutex_unlock(&net_lock);
			}
		}
	}
}

K_THREAD_DEFINE(app_net_tid, 4096, net_thread, NULL, NULL, NULL, 6, 0, 0);

/* ---- services ---- */

int z_impl_svc_net_ready(void)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}

	k_mutex_lock(&net_lock, K_FOREVER);
	bool start = !link_started;

	link_started = true;
	k_mutex_unlock(&net_lock);

	if (start) {
		if (wifi_up() != 0 ||
		    k_sem_take(&net_ready_sem, K_SECONDS(30)) != 0) {
			printk("net: Wi-Fi/IP not ready\n");
			return -EIO;
		}
		for (int i = 0; i < 5 && resolve_broker() != 0; i++) {
			k_sleep(K_SECONDS(1));
		}
		link_ready = broker_resolved;
	} else {
		/* Another application is bringing the link up; wait for it. */
		for (int i = 0; i < 350 && !link_ready; i++) {
			k_sleep(K_MSEC(100));
		}
	}
	return link_ready ? 0 : -EIO;
}

int z_impl_svc_mqtt_open(const char *client_id, const char *pub_topic,
			 const char *sub_topic)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}
	if (!link_ready) {
		return -ENETDOWN;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS) {
		return -EINVAL;
	}

	struct session *s = &sessions[id];

	s->used = true;
	strncpy(s->client_id, client_id, sizeof(s->client_id) - 1);
	strncpy(s->pub_topic, pub_topic, sizeof(s->pub_topic) - 1);
	strncpy(s->sub_topic, sub_topic, sizeof(s->sub_topic) - 1);

	k_mutex_lock(&net_lock, K_FOREVER);
	int rc = session_connect(s);

	k_mutex_unlock(&net_lock);
	return rc;
}

int z_impl_svc_mqtt_publish(const char *payload)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS || !sessions[id].connected) {
		return -EINVAL;
	}

	struct pub_req req = { .id = id };

	strncpy(req.payload, payload, sizeof(req.payload) - 1);
	req.payload[sizeof(req.payload) - 1] = '\0';
	return k_msgq_put(&net_pub_q, &req, K_NO_WAIT);
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_net_ready(void)
{
	return z_impl_svc_net_ready();
}
#include <zephyr/syscalls/svc_net_ready_mrsh.c>

static inline int z_vrfy_svc_mqtt_open(const char *client_id,
				       const char *pub_topic,
				       const char *sub_topic)
{
	char cid[ID_MAX], pt[TOPIC_MAX], st[TOPIC_MAX];

	if (k_usermode_string_copy(cid, (char *)client_id, sizeof(cid)) != 0 ||
	    k_usermode_string_copy(pt, (char *)pub_topic, sizeof(pt)) != 0 ||
	    k_usermode_string_copy(st, (char *)sub_topic, sizeof(st)) != 0) {
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
#endif
