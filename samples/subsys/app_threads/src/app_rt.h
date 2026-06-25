/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_THREADS_APP_RT_H_
#define APP_THREADS_APP_RT_H_

#include <zephyr/kernel.h>

/*
 * App runtime: an application is a GROUP - one memory domain plus one or more
 * threads that share it. The threads see each other's memory (they are one
 * app), are isolated from other apps, and all inherit the app's capability
 * grants. This mirrors a process with multiple threads in a general OS.
 *
 * A loadable app cannot manage stacks itself, so the broker owns a pool of
 * thread slots per app and spawns into the app's domain on request.
 */

#define APP_MAX_THREADS 3   /* per app group */

typedef void (*app_entry_t)(void *arg);

/* Supervisor: create an app group (with the app's private partition), spawn
 * its first (root) thread.
 */
int app_create(const char *name, struct k_mem_partition *part,
	       app_entry_t root, void *arg);

/* Abort the whole app: every thread in the group is reaped together. */
void app_abort_all(void);

/* The SDK call: a running app thread spawns another thread in its own group. */
__syscall int app_thread_spawn(app_entry_t entry, void *arg);

#include <zephyr/syscalls/app_rt.h>

#endif /* APP_THREADS_APP_RT_H_ */
