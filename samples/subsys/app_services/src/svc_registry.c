/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc_registry.h"
#include "app_group.h"

#include <zephyr/kernel.h>

/* Demo service: a trivial counter, proving the brokered-service path. */
K_SEM_DEFINE(demo_token, 0, 1);
static uint32_t demo_calls;

static int demo_handler(struct app_group *caller)
{
	ARG_UNUSED(caller);
	demo_calls++;
	return (int)demo_calls;
}

static const struct svc_desc services[SVC_COUNT] = {
	[SVC_DEMO] = {
		.id = SVC_DEMO,
		.name = "demo-counter",
		.token = &demo_token,
		.handler = demo_handler,
	},
};

const struct svc_desc *svc_registry_get(uint32_t id)
{
	if (id >= SVC_COUNT) {
		return NULL;
	}
	return &services[id];
}

int svc_registry_grant(struct app_group *g, uint32_t id)
{
	const struct svc_desc *svc = svc_registry_get(id);

	if (svc == NULL) {
		return -EINVAL;
	}

	/* The grant happens on the group's thread once it is created; the
	 * bitmask records intent and is applied at start time.
	 */
	g->granted_services |= BIT(id);
	return 0;
}

void svc_registry_revoke_all(struct app_group *g)
{
	for (uint32_t id = 0; id < SVC_COUNT; id++) {
		if ((g->granted_services & BIT(id)) == 0) {
			continue;
		}
		const struct svc_desc *svc = svc_registry_get(id);

		if (svc != NULL && svc->token != NULL) {
			k_object_access_revoke(svc->token, &g->thread);
		}
	}
}

bool svc_registry_granted(struct app_group *g, uint32_t id)
{
	return (g->granted_services & BIT(id)) != 0;
}
