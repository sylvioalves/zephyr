/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_FS_SANDBOX_FS_BROKER_H_
#define APP_FS_SANDBOX_FS_BROKER_H_

#include <zephyr/kernel.h>

/*
 * Per-app sandboxed filesystem - the namespaced-scoped capability. The platform
 * owns one filesystem mount; each app is confined to its own subtree
 * "/lfs/apps/<id>/". An app names files relative to that root and cannot escape
 * it: any path that is absolute or contains a ".." component is rejected before
 * the real filesystem is touched. This is how a phone gives each app its own
 * private storage on one shared filesystem.
 *
 * Files are brokered (open/read/write/close run in supervisor mode) so the
 * confinement is enforced where the app cannot bypass it.
 */

#define FS_MAX_APPS   4
#define FS_PATH_MAX   64
#define FS_OPEN_FILES 4

/* Initialize: mount the filesystem and prepare per-app namespaces. */
int fs_broker_init(void);

/* Register an app thread and give it a numbered namespace. */
int fs_broker_register(k_tid_t tid);

/* Brokered file ops, confined to the caller's namespace. Return a small handle
 * (>=0) from open; negative errno on error (incl. -EACCES for an escape).
 */
int fs_broker_open(const char *rel_path, bool create);
int fs_broker_write(int handle, const char *data, size_t len);
int fs_broker_read(int handle, char *buf, size_t len);
int fs_broker_close(int handle);

#endif /* APP_FS_SANDBOX_FS_BROKER_H_ */
