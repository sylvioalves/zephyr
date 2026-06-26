/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES11_SVC_H_
#define APP_MORE_SERVICES11_SVC_H_

#include <zephyr/kernel.h>

/*
 * An eleventh batch: a raw flash-partition service. The platform owns a flash
 * partition and confines the app to it - every offset is bounds-checked against
 * the partition size, so the app can never read or write outside its granted
 * region. The app erases, writes, and reads back raw bytes from user mode.
 *
 *   svc_flash_erase - erase @len bytes at partition offset @off.
 *   svc_flash_write - write @len bytes at partition offset @off.
 *   svc_flash_read  - read @len bytes from partition offset @off.
 */

/* Scoped: erase @len bytes at @off within the granted partition. */
__syscall int svc_flash_erase(uint32_t off, size_t len);

/* Scoped: write @buf[0..len) at @off within the granted partition. */
__syscall int svc_flash_write(uint32_t off, const uint8_t *buf, size_t len);

/* Scoped: read @len bytes from @off within the granted partition into @buf. */
__syscall int svc_flash_read(uint32_t off, uint8_t *buf, size_t len);

/* Supervisor-only: open the partition before launching the app. */
int svc_flash_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES11_SVC_H_ */
