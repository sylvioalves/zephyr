/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_IPC_SVC_H_
#define APP_IPC_SVC_H_

#include <zephyr/kernel.h>

/*
 * Inter-application messaging, brokered and capability-gated. Isolated apps
 * cannot touch each other's memory, so they cooperate by passing messages
 * through the platform: a sender hands bytes to a named peer, the broker checks
 * the sender's IPC grant and routes the message into the peer's mailbox, and the
 * peer polls for it. Apps never share an address space; only copied bytes cross.
 *
 *   svc_log       - log a line (always available).
 *   svc_ipc_send  - send @len bytes to peer app id @to. Needs CAP_IPC.
 *   svc_ipc_recv  - poll this app's mailbox; -EAGAIN if empty. Needs CAP_IPC.
 */

/* Always available. */
__syscall void svc_log(const char *msg);

/* Scoped (CAP_IPC): deliver @buf[0..len) to peer app @to. Returns bytes sent. */
__syscall int svc_ipc_send(int to, const uint8_t *buf, size_t len);

/*
 * Scoped (CAP_IPC): poll the next message for this app. On success returns the
 * length and writes the sender id to *@from and up to @cap bytes into @buf.
 */
__syscall int svc_ipc_recv(int *from, uint8_t *buf, size_t cap, int32_t timeout_ms);

#include <zephyr/syscalls/svc.h>

#endif /* APP_IPC_SVC_H_ */
