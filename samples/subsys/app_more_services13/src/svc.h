/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_MORE_SERVICES13_SVC_H_
#define APP_MORE_SERVICES13_SVC_H_

#include <zephyr/kernel.h>

/*
 * A thirteenth batch: a hardware DMA memory-copy service. The platform owns a
 * GDMA channel and the DMA-capable source and destination buffers. The app hands
 * over bytes to copy; the supervisor stages them, runs a memory-to-memory DMA
 * transfer, and returns the result. The app never touches the DMA engine or its
 * buffers directly.
 *
 *   svc_dma_copy - DMA-copy @len bytes from @src into @dst via hardware.
 */

/* Scoped: hardware DMA copy of @len bytes from @src to @dst. */
__syscall int svc_dma_copy(const uint8_t *src, uint8_t *dst, size_t len);

/* Supervisor-only: bind the DMA channel before launching the app. */
int svc_dma_install(void);

#include <zephyr/syscalls/svc.h>

#endif /* APP_MORE_SERVICES13_SVC_H_ */
