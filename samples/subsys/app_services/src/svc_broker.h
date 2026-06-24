/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SERVICES_SVC_BROKER_H_
#define APP_SERVICES_SVC_BROKER_H_

#include <zephyr/kernel.h>

/*
 * The broker syscalls: the only surface a user-mode app uses to reach services
 * and to prove liveness. The caller's group is resolved from the running
 * thread (the manager owns it), so no kernel pointer is passed from user mode.
 */

/**
 * @brief Request a service by id. Gated on the caller holding the service's
 * token kobject permission; an ungranted app faults at the syscall boundary
 * (only that app is killed). Returns the service handler's result, or -EPERM.
 */
__syscall int svc_request(uint32_t service_id);

/**
 * @brief Prove liveness: feeds the calling group's watchdog.
 */
__syscall void svc_heartbeat(void);

/**
 * @brief Brokered network probe: open a socket and connect to the network
 * gateway on the given port. Runs in supervisor mode; each app calls it
 * independently and gets its own socket (isolated by kobject permission,
 * bounded by the caller's resource pool). The gateway is an on-subnet,
 * always-routable target, so this proves independent per-app network I/O
 * through the shared stack.
 *
 * @param port   TCP port to connect to on the gateway.
 * @return 0 if the connection succeeded, negative errno otherwise.
 */
__syscall int svc_net_probe(uint16_t port);

#include <zephyr/syscalls/svc_broker.h>

#endif /* APP_SERVICES_SVC_BROKER_H_ */
