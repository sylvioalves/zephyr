/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_FS_SANDBOX_FS_SVC_H_
#define APP_FS_SANDBOX_FS_SVC_H_

#include <zephyr/kernel.h>

/*
 * SDK file API. An app names files relative to its own private root and gets a
 * filesystem view confined to that subtree. Escapes are denied by the broker.
 */

__syscall int app_file_open(const char *path, int create);
__syscall int app_file_write(int handle, const char *data, size_t len);
__syscall int app_file_read(int handle, char *buf, size_t len);
__syscall int app_file_close(int handle);

#include <zephyr/syscalls/fs_svc.h>

#endif /* APP_FS_SANDBOX_FS_SVC_H_ */
