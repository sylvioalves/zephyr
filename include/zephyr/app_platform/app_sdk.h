/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_SDK_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_SDK_H_

#if defined(ZEPHYR_INCLUDE_KERNEL_H_) || defined(ZEPHYR_SYSCALL_LIST_H) ||                        \
	defined(ZEPHYR_INCLUDE_APP_PLATFORM_SERVICES_H_)
#error "an application module must not include kernel.h, a syscall header or app_services.h"
#endif

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/app_platform/app_svc_types.h>
#include <zephyr/app_platform/app_abi_forward.h>
#include <zephyr/llext/symbol.h>

/**
 * @brief Application module SDK
 * @defgroup app_platform_sdk Application module SDK
 * @ingroup app_platform
 * @{
 *
 * The interface a loadable application module is built against. The module
 * calls the services by their app_svc_* names, see
 * @ref app_platform_services, the kernel through app_sleep_ms(),
 * app_sleep_us(), app_uptime_get() and app_yield(), and its heap through
 * app_malloc(), app_calloc(), app_aligned_alloc(), app_realloc() and
 * app_free(). Each is an inline call of a versioned function the firmware
 * exports, app_abi_<name>_v<version>, so the module holds no syscall number
 * and no kernel structure or constant of the firmware it was built against.
 * A module that calls a service the firmware was built without fails to load
 * on an unresolved symbol.
 *
 * A module includes neither kernel.h nor any header that declares syscalls.
 * This header refuses to build after one of them, and with GCC or Clang makes
 * one included after it fail at the poisoned identifier below. A module built
 * with app_platform_add_module() also has APP_PLATFORM_MODULE defined, which
 * makes app_services.h refuse to build.
 */

#if defined(__GNUC__)
#pragma GCC poison ZEPHYR_INCLUDE_KERNEL_H_ ZEPHYR_INCLUDE_APP_PLATFORM_SERVICES_H_
#if !defined(K_SYSCALL_LIMIT)
#pragma GCC poison K_SYSCALL_LIMIT
#endif
#endif

/**
 * @brief Entry point of a loadable application.
 *
 * The module defines it and exports it with EXPORT_SYMBOL(app_main). The
 * platform runs it on the application's thread with the argument given at
 * launch, and the application ends when it returns.
 *
 * @param arg Argument given at launch.
 */
void app_main(void *arg);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_SDK_H_ */
