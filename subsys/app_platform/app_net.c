/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The shared networking and MQTT service. The Wi-Fi interface, the network
 * stack and the socket all live here in the platform; each application gets its
 * own MQTT session keyed by its group id, so several isolated applications
 * publish under their own identity over one shared stack. A supervisor net
 * thread runs every session's socket I/O. Applications hold no socket and link
 * none of this code.
 */

#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_platform.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/net/mqtt.h>
#include "app_grants.h"
#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>
#endif
#include <string.h>
#include <errno.h>

#define BROKER_HOST  CONFIG_APP_PLATFORM_NET_BROKER_HOST
#define BROKER_PORT  1883
#define MAX_SESSIONS CONFIG_APP_PLATFORM_MAX_APPS
#define ID_MAX       24
#define TOPIC_MAX    48
#define PAYLOAD_MAX  64
#define SSID_MAX     33
#define PSK_MAX      65

/* Association attempts, the pause between them, and how long to wait for an
 * address once associated. DHCP on a busy access point can take well over
 * twenty seconds, so the address wait is the generous one.
 */
#define WIFI_CONNECT_TRIES  5
#define WIFI_RETRY_DELAY_S  3
#define WIFI_IP_WAIT_S      45
/* Slack on top of the net thread's budget before an application gives up. */
#define NET_READY_MARGIN_MS 5000

/* Live Wi-Fi credentials, seeded from Kconfig and overridable at runtime. */
static char wifi_ssid[SSID_MAX] = CONFIG_APP_PLATFORM_NET_WIFI_SSID;
static char wifi_psk[PSK_MAX] = CONFIG_APP_PLATFORM_NET_WIFI_PSK;

struct inbox_msg {
	uint16_t len;
	char     data[PAYLOAD_MAX];
};

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
	/* Incoming messages on this session's subscribed topic. */
	struct k_msgq           inbox;
	char                    inbox_buf[4 * sizeof(struct inbox_msg)];
};

static struct session sessions[MAX_SESSIONS];

#define SOCK_PAYLOAD_MAX CONFIG_APP_PLATFORM_NET_DGRAM_MAX

struct sock_msg {
	uint16_t           len;
	uint16_t           src_port;          /* sender's port, host order */
	struct in_addr     src_addr;          /* sender's IPv4 address */
	uint8_t            data[SOCK_PAYLOAD_MAX];
};

/* One platform-owned raw socket per application, keyed by group id. The
 * application never holds the socket; the supervisor net thread pumps incoming
 * bytes into the inbox. The receive lock and staging buffer belong to one
 * application only, and are reset when it is reclaimed.
 */
struct app_sock {
	bool                    used;
	bool                    is_tcp;
	int                     fd;
	struct sockaddr_in      peer;
	struct k_mutex          rx_lock;
	struct sock_msg         rx;
	struct k_msgq           inbox;
	char                    inbox_buf[4 * sizeof(struct sock_msg)];
};

static struct app_sock socks[MAX_SESSIONS];

#define HOST_MAX 64

enum net_op {
	NET_OP_RESOLVE,
	NET_OP_SOCK_OPEN,
	NET_OP_SOCK_SEND,
	NET_OP_SOCK_CLOSE,
	NET_OP_MQTT_OPEN,
};

/* A network call an application hands to the net thread, so the network stack
 * never runs on the application's privileged stack. One slot per application,
 * held by its lock for the length of the call. A reclaim bumps gen so the net
 * thread drops a call whose caller is gone.
 */
struct net_req {
	struct k_mutex          lock;
	struct k_sem            done;
	uint32_t                gen;
	enum net_op             op;
	bool                    tcp;
	uint16_t                port;
	size_t                  len;
	int                     rc;
	struct sockaddr_in      addr;
	char                    host[HOST_MAX];
	uint8_t                 data[SOCK_PAYLOAD_MAX];
};

struct net_req_msg {
	int                     id;
	uint32_t                gen;
};

static struct net_req reqs[MAX_SESSIONS];
K_MSGQ_DEFINE(net_req_q, sizeof(struct net_req_msg), 2 * MAX_SESSIONS, 4);
static K_SEM_DEFINE(net_wake, 0, 1);

struct pub_req {
	int  id;
	char payload[PAYLOAD_MAX];
};
K_MSGQ_DEFINE(net_pub_q, sizeof(struct pub_req), 8, 4);

static K_SEM_DEFINE(net_ready_sem, 0, 1);
static SVC_LOCK_DEFINE(net_lock);
static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback wifi_cb;
static struct sockaddr_storage broker_addr;
static bool link_started;
static bool link_ready;
static bool link_failed;
static bool broker_resolved;
/* Set by app_svc_wifi_connect/disconnect; serviced on the supervisor net thread. */
static bool reconnect_req;
static bool disconnect_req;
static K_SEM_DEFINE(link_change_sem, 0, 1);
/* Cached signal strength, refreshed on the net thread (the status mgmt call
 * cannot run from an application thread without blocking it).
 */
static int8_t link_rssi;

#ifdef CONFIG_SETTINGS
/* Persist the live credentials under "app_net/ssid" and "app_net/psk" so a
 * runtime change made through app_svc_wifi_connect survives a reboot.
 */
static int creds_set(const char *name, size_t len, settings_read_cb read_cb,
		     void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "ssid", &next) && !next) {
		ssize_t n = read_cb(cb_arg, wifi_ssid, sizeof(wifi_ssid) - 1);

		if (n >= 0) {
			wifi_ssid[n] = '\0';
		}
		return 0;
	}
	if (settings_name_steq(name, "psk", &next) && !next) {
		ssize_t n = read_cb(cb_arg, wifi_psk, sizeof(wifi_psk) - 1);

		if (n >= 0) {
			wifi_psk[n] = '\0';
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_net, "app_net", NULL, creds_set, NULL, NULL);

static void creds_save(void)
{
	(void)settings_save_one("app_net/ssid", wifi_ssid, strlen(wifi_ssid));
	(void)settings_save_one("app_net/psk", wifi_psk, strlen(wifi_psk));
}
#else
static void creds_save(void) { }
#endif

/* Losing the address or the association is what tells the platform the shared
 * link is gone: without it link_ready would stay true after the access point
 * disappears, app_svc_wifi_status would keep reporting a live link and the net
 * thread's retry (which is gated on the link being down) would never run
 * again. Clearing the flags here hands recovery back to that retry.
 */
static void link_lost(void)
{
	if (link_ready || link_started) {
		printk("net: link lost - will retry\n");
	}
	link_ready = false;
	link_failed = false;
	broker_resolved = false;
}

static void ipv4_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
		     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (ev) {
	case NET_EVENT_IPV4_ADDR_ADD:
	case NET_EVENT_IPV4_DHCP_BOUND:
		printk("net: IP acquired - shared stack ready\n");
		k_sem_give(&net_ready_sem);
		break;
	case NET_EVENT_IPV4_ADDR_DEL:
		link_lost();
		break;
	default:
		break;
	}
}

/* Wi-Fi events carry a different layer than the IPv4 ones, so they need their
 * own callback: OR-ing the two masks together would corrupt the layer field and
 * the handler would match nothing at all.
 */
static void wifi_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
		     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (ev == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		link_lost();
	}
}

static int wifi_up(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		return -ENODEV;
	}

	static bool cb_added;

	if (!cb_added) {
		net_mgmt_init_event_callback(&ipv4_cb, ipv4_evt,
					     NET_EVENT_IPV4_ADDR_ADD |
					     NET_EVENT_IPV4_DHCP_BOUND |
					     NET_EVENT_IPV4_ADDR_DEL);
		net_mgmt_add_event_callback(&ipv4_cb);
		net_mgmt_init_event_callback(&wifi_cb, wifi_evt,
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
		net_mgmt_add_event_callback(&wifi_cb);
		cb_added = true;
	}

	/* Zero every call: the request carries fields this code does not set
	 * (BSSID, SAE password, timeout), and a static struct would hand the
	 * driver whatever a previous attempt left behind.
	 */
	static struct wifi_connect_req_params p;

	memset(&p, 0, sizeof(p));
	p.ssid = wifi_ssid;
	p.ssid_length = strlen(wifi_ssid);
	p.psk = wifi_psk;
	p.psk_length = strlen(wifi_psk);
	p.security = (p.psk_length > 0) ? WIFI_SECURITY_TYPE_PSK
				       : WIFI_SECURITY_TYPE_NONE;
	p.band = WIFI_FREQ_BAND_UNKNOWN;
	p.channel = WIFI_CHANNEL_ANY;
	p.mfp = WIFI_MFP_OPTIONAL;
	p.timeout = SYS_FOREVER_MS;

	printk("net: connecting to '%s'...\n", wifi_ssid);
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

/* Resolve host:port into an IPv4 sockaddr. Runs in the calling context. */
static int resolve_host(const char *host, uint16_t port, struct sockaddr_in *out)
{
	struct zsock_addrinfo *res, hints = { .ai_family = AF_INET,
					      .ai_socktype = SOCK_DGRAM };
	int rc = zsock_getaddrinfo(host, NULL, &hints, &res);

	if (rc != 0 || !res) {
		return -EHOSTUNREACH;
	}
	memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons(port);
	out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	zsock_freeaddrinfo(res);
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
	} else if (e->type == MQTT_EVT_PUBLISH) {
		/* A message arrived on this session's subscribed topic: read the
		 * payload (bounded by the inbox message) and queue it for the app.
		 */
		const struct mqtt_publish_param *p = &e->param.publish;
		struct inbox_msg m = { 0 };
		int rc = mqtt_read_publish_payload(c, m.data, sizeof(m.data) - 1);

		if (rc >= 0) {
			m.data[rc] = '\0';
			m.len = (uint16_t)rc;
			(void)k_msgq_put(&s->inbox, &m, K_NO_WAIT);
		}
		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id = p->message_id };
			(void)mqtt_publish_qos1_ack(c, &ack);
		}
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

	/* Non-blocking socket: reading a publish payload in the event callback
	 * must not block the shared network thread.
	 */
	int flags = zsock_fcntl(s->client.transport.tcp.sock, F_GETFL, 0);

	(void)zsock_fcntl(s->client.transport.tcp.sock, F_SETFL,
			  flags | O_NONBLOCK);

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

/* Bring the shared Wi-Fi link up. This runs only on the supervisor net thread,
 * never on an application thread: the Wi-Fi connect path must execute from a
 * plain kernel thread (as the stand-alone Wi-Fi samples do), not from inside a
 * service syscall invoked by a user-mode app, or the connect never completes.
 */
static void bring_link_up(void)
{
	if (wifi_ssid[0] == '\0') {
		/* No network configured: stay down until an application asks
		 * for one through the connect service.
		 */
		link_failed = true;
		return;
	}

	struct net_if *iface = net_if_get_first_wifi();

	for (int i = 0; i < 50 &&
	     (iface == NULL || !net_if_is_admin_up(iface)); i++) {
		k_msleep(100);
		iface = net_if_get_first_wifi();
	}
	k_msleep(1000);

	bool got_ip = false;

	/* Retry the association, backing off between attempts. A board that
	 * reboots while it was still associated is refused by the access point
	 * until the old session ages out, and there wifi_up() fails immediately
	 * rather than timing out: without a delay of its own the loop would
	 * spend every attempt inside one second and give up while the access
	 * point is merely busy.
	 */
	for (int attempt = 0; attempt < WIFI_CONNECT_TRIES && !got_ip; attempt++) {
		int wrc = wifi_up();

		/* -EALREADY means the driver still has the previous connect in
		 * flight; that attempt is what we are waiting on, so wait for
		 * the address rather than starting another one.
		 */
		if ((wrc == 0 || wrc == -EALREADY) &&
		    k_sem_take(&net_ready_sem,
			       K_SECONDS(WIFI_IP_WAIT_S)) == 0) {
			got_ip = true;
			break;
		}
		printk("net: Wi-Fi attempt %d got no IP, retrying\n",
		       attempt + 1);
		/* The attempt is spent: clear it so the next one is a fresh
		 * connect rather than another -EALREADY, and give the access
		 * point a moment to release the old session.
		 */
		if (iface != NULL) {
			(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface,
				       NULL, 0);
		}
		k_sleep(K_SECONDS(WIFI_RETRY_DELAY_S));
	}
	if (!got_ip) {
		printk("net: Wi-Fi/IP not ready after retries\n");
		link_failed = true;
		return;
	}
	for (int i = 0; i < 5 && resolve_broker() != 0; i++) {
		k_sleep(K_SECONDS(1));
	}
	link_ready = broker_resolved;
	if (!link_ready) {
		link_failed = true;
	}
}

/* Tear the shared link down. Runs only on the supervisor net thread. */
static void take_link_down(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (sessions[i].connected) {
			(void)mqtt_disconnect(&sessions[i].client, false);
			sessions[i].connected = false;
		}
		sessions[i].used = false;
	}
	if (iface) {
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	}
	link_ready = false;
	link_failed = false;
	link_started = false;
	broker_resolved = false;
}

static int sock_do_open(struct app_sock *sk, const struct net_req *r)
{
	struct sockaddr_in peer;
	int rc = resolve_host(r->host, r->port, &peer);

	if (rc != 0) {
		return rc;
	}
	if (sk->used) {
		return -EALREADY;
	}

	int fd = zsock_socket(AF_INET, r->tcp ? SOCK_STREAM : SOCK_DGRAM,
			      r->tcp ? IPPROTO_TCP : IPPROTO_UDP);

	if (fd < 0) {
		return -errno;
	}
	if (r->tcp && zsock_connect(fd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
		/* Report what actually went wrong: a flat -ECONNREFUSED
		 * hides a routing or resource failure behind the one
		 * error that looks like the peer's fault.
		 */
		rc = -errno;
		zsock_close(fd);
		return rc;
	}
	int flags = zsock_fcntl(fd, F_GETFL, 0);

	(void)zsock_fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	sk->fd = fd;
	sk->is_tcp = r->tcp;
	sk->peer = peer;
	k_msgq_init(&sk->inbox, sk->inbox_buf, sizeof(struct sock_msg), 4);
	sk->used = true;
	return 0;
}

static int sock_do_send(struct app_sock *sk, const struct net_req *r)
{
	ssize_t n;

	if (!sk->used || sk->fd < 0 || sk->is_tcp != r->tcp) {
		return -ENOTCONN;
	}
	if (r->tcp) {
		n = zsock_send(sk->fd, r->data, r->len, 0);
	} else {
		n = zsock_sendto(sk->fd, r->data, r->len, 0,
				 (struct sockaddr *)&sk->peer, sizeof(sk->peer));
	}
	return (n < 0) ? -errno : (int)n;
}

static int sock_do_close(struct app_sock *sk)
{
	if (sk->fd >= 0) {
		zsock_close(sk->fd);
		sk->fd = -1;
	}
	sk->used = false;
	return 0;
}

static int net_run(int id, struct net_req *r)
{
	switch (r->op) {
	case NET_OP_RESOLVE:
		return resolve_host(r->host, 0, &r->addr);
	case NET_OP_SOCK_OPEN:
		return sock_do_open(&socks[id], r);
	case NET_OP_SOCK_SEND:
		return sock_do_send(&socks[id], r);
	case NET_OP_SOCK_CLOSE:
		return sock_do_close(&socks[id]);
	case NET_OP_MQTT_OPEN:
		return session_connect(&sessions[id]);
	default:
		return -EINVAL;
	}
}

static void net_serve_requests(void)
{
	struct net_req_msg m;

	while (k_msgq_get(&net_req_q, &m, K_NO_WAIT) == 0) {
		if (m.id < 0 || m.id >= MAX_SESSIONS) {
			continue;
		}

		struct net_req *r = &reqs[m.id];

		if (app_service_lock(&net_lock, K_FOREVER) != 0) {
			r->rc = -EIO;
			k_sem_give(&r->done);
			continue;
		}
		if (r->gen == m.gen) {
			r->rc = net_run(m.id, r);
			k_sem_give(&r->done);
		}
		app_service_unlock(&net_lock);
	}
}

/* Run the call staged in the caller's slot on the net thread and wait for its
 * result. The caller holds the slot's lock.
 */
static int net_call(int id)
{
	struct net_req *r = &reqs[id];
	struct net_req_msg m = { .id = id, .gen = r->gen };

	k_sem_reset(&r->done);
	if (k_msgq_put(&net_req_q, &m, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	k_sem_give(&net_wake);
	(void)k_sem_take(&r->done, K_FOREVER);
	return r->rc;
}

static void net_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

#ifdef CONFIG_SETTINGS
	if (settings_subsys_init() == 0) {
		(void)settings_load_subtree("app_net");
	}
#endif

	for (int i = 0; i < MAX_SESSIONS; i++) {
		socks[i].fd = -1;
	}

	for (;;) {
		struct pub_req req;

		if (disconnect_req) {
			disconnect_req = false;
			take_link_down();
			k_sem_give(&link_change_sem);
		}
		if (reconnect_req) {
			reconnect_req = false;
			take_link_down();
			link_started = true;
			bring_link_up();
			k_sem_give(&link_change_sem);
		}

		if (link_started && !link_ready && !link_failed) {
			bring_link_up();
		}

		(void)k_sem_take(&net_wake, K_MSEC(500));
		net_serve_requests();

		while (k_msgq_get(&net_pub_q, &req, K_NO_WAIT) == 0) {
			if (req.id < 0 || req.id >= MAX_SESSIONS ||
			    !sessions[req.id].connected) {
				continue;
			}
			struct session *s = &sessions[req.id];
			struct mqtt_publish_param p = { 0 };

			p.message.topic.topic.utf8 = s->pub_topic;
			p.message.topic.topic.size = strlen(s->pub_topic);
			p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
			p.message.payload.data = (uint8_t *)req.payload;
			p.message.payload.len = strlen(req.payload);

			if (app_service_lock(&net_lock, K_FOREVER) != 0) {
				continue;
			}
			int rc = mqtt_publish(&s->client, &p);

			app_service_unlock(&net_lock);
			printk("mqtt: '%s' published '%s' rc=%d\n",
			       s->client_id, req.payload, rc);
		}

		/* The reaper releases a session under net_lock, so the flag
		 * and the socket it guards are read inside the same hold: a
		 * session sampled as connected outside it can be torn down
		 * before the socket is used. Polling with no timeout keeps the
		 * hold short.
		 */
		for (int i = 0; i < MAX_SESSIONS; i++) {
			struct session *s = &sessions[i];

			if (app_service_lock(&net_lock, K_FOREVER) != 0) {
				continue;
			}
			if (s->connected) {
				struct zsock_pollfd fd = {
					.fd = s->client.transport.tcp.sock,
					.events = ZSOCK_POLLIN };

				if (zsock_poll(&fd, 1, 0) > 0) {
					mqtt_input(&s->client);
				}
				mqtt_live(&s->client);
			}
			app_service_unlock(&net_lock);
		}

		/* Pump each open raw socket: drain ready bytes into its inbox.
		 * Held under net_lock for the same reason as the sessions
		 * above: the descriptor must not be closed between the test
		 * and the receive.
		 */
		for (int i = 0; i < MAX_SESSIONS; i++) {
			struct app_sock *sk = &socks[i];

			if (app_service_lock(&net_lock, K_FOREVER) != 0) {
				continue;
			}
			if (!sk->used || sk->fd < 0) {
				app_service_unlock(&net_lock);
				continue;
			}
			struct zsock_pollfd fd = { .fd = sk->fd,
						   .events = ZSOCK_POLLIN };

			if (zsock_poll(&fd, 1, 0) <= 0) {
				app_service_unlock(&net_lock);
				continue;
			}
			static struct sock_msg m;
			struct sockaddr_in from = { 0 };
			socklen_t fromlen = sizeof(from);
			int n = zsock_recvfrom(sk->fd, m.data, sizeof(m.data), 0,
					       (struct sockaddr *)&from, &fromlen);
			bool is_tcp = sk->is_tcp;
			struct k_msgq *inbox = &sk->inbox;

			app_service_unlock(&net_lock);

			if (n > 0) {
				m.len = (uint16_t)n;
				m.src_addr = from.sin_addr;
				m.src_port = ntohs(from.sin_port);
				(void)k_msgq_put(inbox, &m, K_NO_WAIT);
			} else if (n == 0 && is_tcp) {
				/* Peer closed the stream: signal end with a
				 * zero-length message so the reader sees it.
				 */
				m.len = 0;
				(void)k_msgq_put(inbox, &m, K_NO_WAIT);
			}
		}

		/* Refresh the cached signal strength roughly every two seconds.
		 * Querying the interface here keeps the blocking mgmt call off the
		 * application threads.
		 */
		static int64_t rssi_next;

		if (link_ready && k_uptime_get() >= rssi_next) {
			rssi_next = k_uptime_get() + 2000;
			struct net_if *iface = net_if_get_first_wifi();
			struct wifi_iface_status st = { 0 };

			if (iface != NULL &&
			    net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st,
				     sizeof(st)) == 0) {
				link_rssi = (int8_t)st.rssi;
			}
		}
		k_msleep(20);
	}
}

K_THREAD_DEFINE(app_net_tid, 4096, net_thread, NULL, NULL, NULL, 6, 0, 0);

/* ---- services ---- */

int z_impl_app_svc_net_ready(void)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}

	/* Ask the supervisor net thread to bring the shared link up, then wait
	 * for the result. The connect itself must not run on this app thread.
	 */
	if (app_service_lock(&net_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	link_started = true;
	app_service_unlock(&net_lock);

	/* Wait for the net thread's own budget to run out, not less: it makes
	 * WIFI_CONNECT_TRIES attempts, each up to WIFI_IP_WAIT_S plus the pause
	 * between them. Returning earlier would report a failure while the
	 * platform was still associating.
	 */
	const int wait_ms = WIFI_CONNECT_TRIES *
			    ((WIFI_IP_WAIT_S + WIFI_RETRY_DELAY_S) * 1000) +
			    NET_READY_MARGIN_MS;

	for (int i = 0; i < wait_ms / 100 && !link_ready && !link_failed; i++) {
		k_sleep(K_MSEC(100));
	}
	return link_ready ? 0 : -EIO;
}

int z_impl_app_svc_mqtt_open(const char *client_id, const char *pub_topic,
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
	k_msgq_init(&s->inbox, s->inbox_buf, sizeof(struct inbox_msg), 4);

	struct net_req *r = &reqs[id];

	if (k_mutex_lock(&r->lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	r->op = NET_OP_MQTT_OPEN;

	int rc = net_call(id);

	k_mutex_unlock(&r->lock);
	return rc;
}

int z_impl_app_svc_mqtt_recv(char *payload, size_t cap, uint32_t timeout_ms)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS || !sessions[id].used) {
		return -EINVAL;
	}

	struct inbox_msg m;
	int rc = k_msgq_get(&sessions[id].inbox, &m, K_MSEC(timeout_ms));

	if (rc != 0) {
		return -EAGAIN;
	}

	size_t n = MIN(m.len, cap - 1);

	memcpy(payload, m.data, n);
	payload[n] = '\0';
	return (int)n;
}

int z_impl_app_svc_mqtt_publish(const char *payload)
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

	int rc = k_msgq_put(&net_pub_q, &req, K_NO_WAIT);

	k_sem_give(&net_wake);
	return rc;
}

int z_impl_app_svc_wifi_status(struct app_svc_wifi_status *out)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}

	memset(out, 0, sizeof(*out));
	out->connected = link_ready ? 1 : 0;
	out->rssi = link_rssi;
	strncpy(out->ssid, wifi_ssid, sizeof(out->ssid) - 1);

	/* Read only cached interface state. A WIFI_IFACE_STATUS mgmt round-trip
	 * would query the driver and can block in this app-thread context, so the
	 * signal strength comes from the value the net thread caches instead.
	 */
	struct net_if *iface = net_if_get_first_wifi();

	if (iface != NULL) {
		struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

		if (ipv4 != NULL) {
			ARRAY_FOR_EACH(ipv4->unicast, i) {
				if (ipv4->unicast[i].ipv4.is_used) {
					net_addr_ntop(AF_INET,
						&ipv4->unicast[i].ipv4.address.in_addr,
						out->ipv4, sizeof(out->ipv4));
					break;
				}
			}
		}
	}
	return 0;
}

int z_impl_app_svc_wifi_connect(const char *ssid, const char *psk)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}
	if (ssid[0] == '\0') {
		return -EINVAL;
	}

	if (app_service_lock(&net_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
	wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
	strncpy(wifi_psk, psk, sizeof(wifi_psk) - 1);
	wifi_psk[sizeof(wifi_psk) - 1] = '\0';
	creds_save();
	k_sem_reset(&link_change_sem);
	reconnect_req = true;
	app_service_unlock(&net_lock);

	(void)k_sem_take(&link_change_sem, K_SECONDS(90));
	return link_ready ? 0 : -EIO;
}

int z_impl_app_svc_wifi_disconnect(void)
{
	if (!app_has_cap(APP_CAP_NET)) {
		return -EPERM;
	}

	if (app_service_lock(&net_lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	k_sem_reset(&link_change_sem);
	disconnect_req = true;
	app_service_unlock(&net_lock);

	(void)k_sem_take(&link_change_sem, K_SECONDS(10));
	return 0;
}

/* ---- raw UDP/TCP datagram services ---- */

int z_impl_app_svc_resolve(const char *host, char *out_ip, size_t cap)
{
	if (!app_has_cap(APP_CAP_SOCKET)) {
		return -EPERM;
	}
	if (!link_ready) {
		return -ENETDOWN;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS) {
		return -EINVAL;
	}

	struct net_req *r = &reqs[id];

	if (k_mutex_lock(&r->lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	r->op = NET_OP_RESOLVE;
	strncpy(r->host, host, sizeof(r->host) - 1);
	r->host[sizeof(r->host) - 1] = '\0';

	int rc = net_call(id);
	struct in_addr addr = r->addr.sin_addr;

	k_mutex_unlock(&r->lock);
	if (rc != 0) {
		return rc;
	}
	if (zsock_inet_ntop(AF_INET, &addr, out_ip, cap) == NULL) {
		return -ENOSPC;
	}
	return 0;
}

static int sock_open(bool tcp, const char *host, uint16_t port)
{
	if (!app_has_cap(APP_CAP_SOCKET)) {
		return -EPERM;
	}
	if (!link_ready) {
		return -ENETDOWN;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS) {
		return -EINVAL;
	}
	if (socks[id].used) {
		return -EALREADY;
	}

	struct net_req *r = &reqs[id];

	if (k_mutex_lock(&r->lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	r->op = NET_OP_SOCK_OPEN;
	r->tcp = tcp;
	r->port = port;
	strncpy(r->host, host, sizeof(r->host) - 1);
	r->host[sizeof(r->host) - 1] = '\0';

	int rc = net_call(id);

	k_mutex_unlock(&r->lock);
	return rc;
}

static int sock_send(bool tcp, const void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_SOCKET)) {
		return -EPERM;
	}
	if (!link_ready) {
		return -ENETDOWN;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS) {
		return -EINVAL;
	}

	struct app_sock *sk = &socks[id];

	if (!sk->used || sk->fd < 0 || sk->is_tcp != tcp) {
		return -ENOTCONN;
	}
	if (!tcp && len > SOCK_PAYLOAD_MAX) {
		return -EMSGSIZE;
	}

	struct net_req *r = &reqs[id];

	if (k_mutex_lock(&r->lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}

	size_t sent = 0U;
	int rc = 0;

	/* A stream is sent in bounded chunks, a datagram in one piece. */
	do {
		size_t chunk = MIN(len - sent, sizeof(r->data));

		memcpy(r->data, (const uint8_t *)buf + sent, chunk);
		r->op = NET_OP_SOCK_SEND;
		r->tcp = tcp;
		r->len = chunk;
		rc = net_call(id);
		if (rc < 0) {
			break;
		}
		sent += (size_t)rc;
		if ((size_t)rc < chunk) {
			break;
		}
	} while (sent < len);

	k_mutex_unlock(&r->lock);
	return (sent > 0U) ? (int)sent : rc;
}

static int sock_recv(bool tcp, void *buf, size_t cap, uint32_t timeout_ms,
		     struct in_addr *src_addr, uint16_t *src_port)
{
	if (!app_has_cap(APP_CAP_SOCKET)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS) {
		return -EINVAL;
	}

	struct app_sock *sk = &socks[id];

	if (!sk->used || sk->is_tcp != tcp) {
		return -ENOTCONN;
	}

	k_timepoint_t end = sys_timepoint_calc(K_MSEC(timeout_ms));

	if (k_mutex_lock(&sk->rx_lock, K_MSEC(timeout_ms)) != 0) {
		return -EAGAIN;
	}
	if (k_msgq_get(&sk->inbox, &sk->rx, sys_timepoint_timeout(end)) != 0) {
		k_mutex_unlock(&sk->rx_lock);
		return -EAGAIN;
	}

	if (src_addr != NULL) {
		*src_addr = sk->rx.src_addr;
	}
	if (src_port != NULL) {
		*src_port = sk->rx.src_port;
	}

	size_t n = MIN((size_t)sk->rx.len, cap);

	memcpy(buf, sk->rx.data, n);
	k_mutex_unlock(&sk->rx_lock);
	return (int)n;
}

static int sock_close(bool tcp)
{
	if (!app_has_cap(APP_CAP_SOCKET)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0 || id >= MAX_SESSIONS) {
		return -EINVAL;
	}

	struct app_sock *sk = &socks[id];

	if (!sk->used || sk->is_tcp != tcp) {
		return -ENOTCONN;
	}

	struct net_req *r = &reqs[id];

	if (k_mutex_lock(&r->lock, SVC_LOCK_TIMEOUT) != 0) {
		return -EBUSY;
	}
	r->op = NET_OP_SOCK_CLOSE;

	int rc = net_call(id);

	k_mutex_unlock(&r->lock);
	return rc;
}

int z_impl_app_svc_udp_open(const char *host, uint16_t port)
{
	return sock_open(false, host, port);
}

int z_impl_app_svc_udp_send(const void *buf, size_t len)
{
	return sock_send(false, buf, len);
}

int z_impl_app_svc_udp_recv(void *buf, size_t cap, uint32_t timeout_ms)
{
	return sock_recv(false, buf, cap, timeout_ms, NULL, NULL);
}

int z_impl_app_svc_udp_recv_from(void *buf, size_t cap, uint32_t timeout_ms,
				 char *src_ip, size_t ip_cap, uint16_t *src_port)
{
	struct in_addr addr = { 0 };
	uint16_t port = 0;
	int rc = sock_recv(false, buf, cap, timeout_ms, &addr, &port);

	if (rc < 0) {
		return rc;
	}
	if (src_ip != NULL && ip_cap > 0) {
		if (zsock_inet_ntop(AF_INET, &addr, src_ip, ip_cap) == NULL) {
			src_ip[0] = '\0';
		}
	}
	if (src_port != NULL) {
		*src_port = port;
	}
	return rc;
}

int z_impl_app_svc_udp_close(void)
{
	return sock_close(false);
}

int z_impl_app_svc_tcp_connect(const char *host, uint16_t port)
{
	return sock_open(true, host, port);
}

int z_impl_app_svc_tcp_send(const void *buf, size_t len)
{
	return sock_send(true, buf, len);
}

int z_impl_app_svc_tcp_recv(void *buf, size_t cap, uint32_t timeout_ms)
{
	return sock_recv(true, buf, cap, timeout_ms, NULL, NULL);
}

int z_impl_app_svc_tcp_close(void)
{
	return sock_close(true);
}

/* Release a reclaimed application's network resources: its MQTT session and any
 * raw socket it left open. Registered as a platform reclaim hook, so it runs on
 * the reaper-helper thread (never the supervisor) when an app is reclaimed.
 */
static void net_release(int id)
{
	if (id < 0 || id >= MAX_SESSIONS) {
		return;
	}

	if (app_service_lock(&net_lock, K_FOREVER) != 0) {
		return;
	}
	if (sessions[id].used) {
		if (sessions[id].connected) {
			(void)mqtt_disconnect(&sessions[id].client, false);
		}
		sessions[id] = (struct session){ 0 };
	}
	if (socks[id].used) {
		if (socks[id].fd >= 0) {
			(void)zsock_close(socks[id].fd);
		}
		socks[id].used = false;
		socks[id].fd = -1;
	}
	k_mutex_init(&socks[id].rx_lock);
	reqs[id].gen++;
	k_mutex_init(&reqs[id].lock);
	k_sem_init(&reqs[id].done, 0, 1);
	app_service_unlock(&net_lock);
}

static int net_service_init(void)
{
	for (int i = 0; i < MAX_SESSIONS; i++) {
		k_mutex_init(&socks[i].rx_lock);
		k_mutex_init(&reqs[i].lock);
		k_sem_init(&reqs[i].done, 0, 1);
	}
	return app_platform_on_reclaim(net_release);
}
SYS_INIT(net_service_init, APPLICATION, 99);

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_app_svc_net_ready(void)
{
	return z_impl_app_svc_net_ready();
}
#include <zephyr/syscalls/app_svc_net_ready_mrsh.c>

static inline int z_vrfy_app_svc_mqtt_open(const char *client_id,
					   const char *pub_topic,
					   const char *sub_topic)
{
	char cid[ID_MAX], pt[TOPIC_MAX], st[TOPIC_MAX];

	if (k_usermode_string_copy(cid, (char *)client_id, sizeof(cid)) != 0 ||
	    k_usermode_string_copy(pt, (char *)pub_topic, sizeof(pt)) != 0 ||
	    k_usermode_string_copy(st, (char *)sub_topic, sizeof(st)) != 0) {
		return -EFAULT;
	}
	return z_impl_app_svc_mqtt_open(cid, pt, st);
}
#include <zephyr/syscalls/app_svc_mqtt_open_mrsh.c>

static inline int z_vrfy_app_svc_mqtt_publish(const char *payload)
{
	char buf[PAYLOAD_MAX];

	if (k_usermode_string_copy(buf, (char *)payload, sizeof(buf)) != 0) {
		return -EFAULT;
	}
	return z_impl_app_svc_mqtt_publish(buf);
}
#include <zephyr/syscalls/app_svc_mqtt_publish_mrsh.c>

static inline int z_vrfy_app_svc_mqtt_recv(char *payload, size_t cap,
					   uint32_t timeout_ms)
{
	char buf[PAYLOAD_MAX];

	if (cap == 0 || cap > sizeof(buf)) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(payload, cap));

	int rc = z_impl_app_svc_mqtt_recv(buf, cap, timeout_ms);

	if (rc > 0 && k_usermode_to_copy(payload, buf, rc + 1) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/app_svc_mqtt_recv_mrsh.c>

static inline int z_vrfy_app_svc_wifi_status(struct app_svc_wifi_status *out)
{
	struct app_svc_wifi_status st;

	K_OOPS(K_SYSCALL_MEMORY_WRITE(out, sizeof(*out)));

	int rc = z_impl_app_svc_wifi_status(&st);

	if (rc == 0 && k_usermode_to_copy(out, &st, sizeof(st)) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/app_svc_wifi_status_mrsh.c>

static inline int z_vrfy_app_svc_wifi_connect(const char *ssid, const char *psk)
{
	char s[SSID_MAX], p[PSK_MAX];

	if (k_usermode_string_copy(s, (char *)ssid, sizeof(s)) != 0 ||
	    k_usermode_string_copy(p, (char *)psk, sizeof(p)) != 0) {
		return -EFAULT;
	}
	return z_impl_app_svc_wifi_connect(s, p);
}
#include <zephyr/syscalls/app_svc_wifi_connect_mrsh.c>

static inline int z_vrfy_app_svc_wifi_disconnect(void)
{
	return z_impl_app_svc_wifi_disconnect();
}
#include <zephyr/syscalls/app_svc_wifi_disconnect_mrsh.c>

static inline int z_vrfy_app_svc_resolve(const char *host, char *out_ip, size_t cap)
{
	char h[64];

	if (cap == 0 || cap > 64) {
		return -EINVAL;
	}
	if (k_usermode_string_copy(h, (char *)host, sizeof(h)) != 0) {
		return -EFAULT;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(out_ip, cap));

	char buf[64];
	int rc = z_impl_app_svc_resolve(h, buf, cap);

	if (rc == 0 && k_usermode_to_copy(out_ip, buf, strlen(buf) + 1) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/app_svc_resolve_mrsh.c>

static inline int z_vrfy_app_svc_udp_open(const char *host, uint16_t port)
{
	char h[64];

	if (k_usermode_string_copy(h, (char *)host, sizeof(h)) != 0) {
		return -EFAULT;
	}
	return z_impl_app_svc_udp_open(h, port);
}
#include <zephyr/syscalls/app_svc_udp_open_mrsh.c>

static inline int z_vrfy_app_svc_udp_send(const void *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_app_svc_udp_send(buf, len);
}
#include <zephyr/syscalls/app_svc_udp_send_mrsh.c>

static inline int z_vrfy_app_svc_udp_recv(void *buf, size_t cap, uint32_t timeout_ms)
{
	if (cap == 0) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	return z_impl_app_svc_udp_recv(buf, cap, timeout_ms);
}
#include <zephyr/syscalls/app_svc_udp_recv_mrsh.c>

static inline int z_vrfy_app_svc_udp_recv_from(void *buf, size_t cap,
					       uint32_t timeout_ms, char *src_ip,
					       size_t ip_cap, uint16_t *src_port)
{
	if (cap == 0) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	if (src_ip != NULL) {
		K_OOPS(K_SYSCALL_MEMORY_WRITE(src_ip, ip_cap));
	}
	if (src_port != NULL) {
		K_OOPS(K_SYSCALL_MEMORY_WRITE(src_port, sizeof(*src_port)));
	}
	return z_impl_app_svc_udp_recv_from(buf, cap, timeout_ms, src_ip, ip_cap,
					    src_port);
}
#include <zephyr/syscalls/app_svc_udp_recv_from_mrsh.c>

static inline int z_vrfy_app_svc_udp_close(void)
{
	return z_impl_app_svc_udp_close();
}
#include <zephyr/syscalls/app_svc_udp_close_mrsh.c>

static inline int z_vrfy_app_svc_tcp_connect(const char *host, uint16_t port)
{
	char h[64];

	if (k_usermode_string_copy(h, (char *)host, sizeof(h)) != 0) {
		return -EFAULT;
	}
	return z_impl_app_svc_tcp_connect(h, port);
}
#include <zephyr/syscalls/app_svc_tcp_connect_mrsh.c>

static inline int z_vrfy_app_svc_tcp_send(const void *buf, size_t len)
{
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_app_svc_tcp_send(buf, len);
}
#include <zephyr/syscalls/app_svc_tcp_send_mrsh.c>

static inline int z_vrfy_app_svc_tcp_recv(void *buf, size_t cap, uint32_t timeout_ms)
{
	if (cap == 0) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	return z_impl_app_svc_tcp_recv(buf, cap, timeout_ms);
}
#include <zephyr/syscalls/app_svc_tcp_recv_mrsh.c>

static inline int z_vrfy_app_svc_tcp_close(void)
{
	return z_impl_app_svc_tcp_close();
}
#include <zephyr/syscalls/app_svc_tcp_close_mrsh.c>
#endif
