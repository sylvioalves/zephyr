/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc_broker.h"
#include "svc_registry.h"
#include "app_group.h"

#include <zephyr/kernel.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include "net_link.h"

int z_impl_svc_request(uint32_t service_id)
{
	const struct svc_desc *svc = svc_registry_get(service_id);
	struct app_group *g = app_group_current();

	if (svc == NULL || svc->handler == NULL) {
		return -EINVAL;
	}
	return svc->handler(g);
}

void z_impl_svc_heartbeat(void)
{
	struct app_group *g = app_group_current();

	if (g != NULL) {
		app_group_heartbeat(g);
	}
}

int z_impl_svc_net_probe(uint16_t port)
{
	struct net_sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};

	memcpy(&addr.sin_addr, net_link_gateway, 4);

	/*
	 * Runs in supervisor mode: open a socket and connect to the target, to
	 * prove this app independently exercises the shared IP stack. Each app
	 * gets its own net_context (isolated by kobject permission); the per-app
	 * resource pool bounds the allocation.
	 */
	int sock = zsock_socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0) {
		return -errno;
	}

	int rc = zsock_connect(sock, (struct net_sockaddr *)&addr, sizeof(addr));

	zsock_close(sock);
	return (rc == 0) ? 0 : -errno;
}

#ifdef CONFIG_USERSPACE

static inline int z_vrfy_svc_request(uint32_t service_id)
{
	const struct svc_desc *svc = svc_registry_get(service_id);

	/*
	 * Per-app service gate: the caller must hold permission on the
	 * service's token kobject. An ungranted app fails here (K_OOPS kills
	 * only that app).
	 */
	if (svc == NULL || svc->token == NULL) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_OBJ(svc->token, K_OBJ_SEM));
	return z_impl_svc_request(service_id);
}
#include <zephyr/syscalls/svc_request_mrsh.c>

static inline void z_vrfy_svc_heartbeat(void)
{
	z_impl_svc_heartbeat();
}
#include <zephyr/syscalls/svc_heartbeat_mrsh.c>

static inline int z_vrfy_svc_net_probe(uint16_t port)
{
	return z_impl_svc_net_probe(port);
}
#include <zephyr/syscalls/svc_net_probe_mrsh.c>

#endif /* CONFIG_USERSPACE */
