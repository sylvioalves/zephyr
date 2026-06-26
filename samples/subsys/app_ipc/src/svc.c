/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc.h"
#include "app_caps.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/app_platform/app_platform.h>
#include <string.h>
#include <errno.h>

#define IPC_MSG_MAX  32
#define MAILBOX_DEPTH 8

/* A message in transit: the sender id plus the copied payload. */
struct ipc_msg {
	int      from;
	uint16_t len;
	uint8_t  data[IPC_MSG_MAX];
};

/*
 * One mailbox per application group. The broker owns these in supervisor mode;
 * apps never see another app's mailbox, only the bytes routed into their own.
 */
static struct k_msgq mailboxes[CONFIG_APP_PLATFORM_MAX_APPS];
static char mailbox_bufs[CONFIG_APP_PLATFORM_MAX_APPS]
			[MAILBOX_DEPTH * sizeof(struct ipc_msg)];

void svc_ipc_init(void)
{
	for (int i = 0; i < CONFIG_APP_PLATFORM_MAX_APPS; i++) {
		k_msgq_init(&mailboxes[i], mailbox_bufs[i], sizeof(struct ipc_msg),
			    MAILBOX_DEPTH);
	}
}

void z_impl_svc_log(const char *msg)
{
	printk("    [app %d] %s\n", app_platform_id_current(), msg);
}

int z_impl_svc_ipc_send(int to, const uint8_t *buf, size_t len)
{
	if ((app_platform_caps_current() & CAP_IPC) == 0U) {
		return -EPERM;
	}
	if (to < 0 || to >= CONFIG_APP_PLATFORM_MAX_APPS || len == 0U ||
	    len > IPC_MSG_MAX) {
		return -EINVAL;
	}

	struct ipc_msg m = { .from = app_platform_id_current(), .len = (uint16_t)len };

	memcpy(m.data, buf, len);
	if (k_msgq_put(&mailboxes[to], &m, K_NO_WAIT) != 0) {
		return -ENOBUFS;
	}
	return (int)len;
}

int z_impl_svc_ipc_recv(int *from, uint8_t *buf, size_t cap, int32_t timeout_ms)
{
	if ((app_platform_caps_current() & CAP_IPC) == 0U) {
		return -EPERM;
	}

	int me = app_platform_id_current();

	if (me < 0) {
		return -ESRCH;
	}

	struct ipc_msg m;
	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);

	if (k_msgq_get(&mailboxes[me], &m, t) != 0) {
		return -EAGAIN;
	}

	size_t n = MIN((size_t)m.len, cap);

	*from = m.from;
	memcpy(buf, m.data, n);
	return (int)n;
}

#ifdef CONFIG_USERSPACE
static inline void z_vrfy_svc_log(const char *msg)
{
	char kbuf[48];

	if (k_usermode_string_copy(kbuf, (char *)msg, sizeof(kbuf)) == 0) {
		z_impl_svc_log(kbuf);
	}
}
#include <zephyr/syscalls/svc_log_mrsh.c>

static inline int z_vrfy_svc_ipc_send(int to, const uint8_t *buf, size_t len)
{
	if (len > IPC_MSG_MAX) {
		return -EINVAL;
	}
	K_OOPS(K_SYSCALL_MEMORY_READ(buf, len));
	return z_impl_svc_ipc_send(to, buf, len);
}
#include <zephyr/syscalls/svc_ipc_send_mrsh.c>

static inline int z_vrfy_svc_ipc_recv(int *from, uint8_t *buf, size_t cap,
				      int32_t timeout_ms)
{
	if (cap > IPC_MSG_MAX) {
		cap = IPC_MSG_MAX;
	}
	K_OOPS(K_SYSCALL_MEMORY_WRITE(from, sizeof(*from)));
	K_OOPS(K_SYSCALL_MEMORY_WRITE(buf, cap));
	return z_impl_svc_ipc_recv(from, buf, cap, timeout_ms);
}
#include <zephyr/syscalls/svc_ipc_recv_mrsh.c>
#endif
