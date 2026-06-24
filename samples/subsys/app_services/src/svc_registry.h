/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SERVICES_SVC_REGISTRY_H_
#define APP_SERVICES_SVC_REGISTRY_H_

#include <zephyr/kernel.h>

struct app_group;

/* Service identifiers (bit positions in the per-app grant bitmask). */
enum svc_id {
	SVC_DEMO = 0,     /* a trivial counter service, for M1 */
	SVC_COUNT,
};

/*
 * A service descriptor. The token is the kobject whose permission represents
 * access to this service; granting it to an app's thread is what lets the
 * broker syscall through. The handler runs in supervisor mode.
 */
struct svc_desc {
	enum svc_id   id;
	const char   *name;
	struct k_sem *token;
	int (*handler)(struct app_group *caller);
};

/* Look up a service by id, or NULL. */
const struct svc_desc *svc_registry_get(uint32_t id);

/* Grant / revoke a service to an app group (token kobject + bitmask). */
int  svc_registry_grant(struct app_group *g, uint32_t id);
void svc_registry_revoke_all(struct app_group *g);

/* True if the group currently holds the grant for id. */
bool svc_registry_granted(struct app_group *g, uint32_t id);

#endif /* APP_SERVICES_SVC_REGISTRY_H_ */
