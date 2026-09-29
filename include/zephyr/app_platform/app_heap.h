/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_HEAP_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_HEAP_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_heap.h>

/**
 * @brief Per-application heap
 * @defgroup app_platform_heap Per-application heap
 * @ingroup app_platform
 * @{
 *
 * An application that declares a heap size is given a region of internal RAM,
 * mapped into its own memory domain, for as long as it is loaded. Before the
 * application runs, the platform zeroes the region and initializes a heap at
 * its start. From then on only the application touches the region.
 */

/** Start of a heap region. The heap memory follows it. */
struct app_heap_region {
	atomic_t lock;        /**< taken by an application thread inside the allocator */
	struct sys_heap heap; /**< heap over the rest of the region */
};

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_HEAP_H_ */
