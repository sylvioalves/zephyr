/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_

#include <zephyr/llext/llext.h>

/**
 * @brief Application loader
 * @ingroup app_platform
 * @{
 */

/**
 * @brief Verify a module by SHA-256 and load it as an llext.
 *
 * The hash is checked before the module is loaded, so a tampered or corrupted
 * module is rejected and never linked. On success the entry symbol named
 * @p entry_sym is resolved.
 *
 * @param name      Name for the loaded extension.
 * @param buf       Module bytes.
 * @param len       Module length.
 * @param expect    Expected 32-byte SHA-256 of the module.
 * @param entry_sym Entry symbol name to resolve.
 * @param[out] ext  Receives the loaded extension on success.
 * @param[out] entry Receives the resolved entry pointer on success.
 *
 * @retval 0 on success.
 * @retval -EPERM if the hash does not match.
 * @retval -ENOENT if the entry symbol is not found.
 * @retval other negative errno from crypto or llext on failure.
 */
int app_loader_verify_load(const char *name, const uint8_t *buf, size_t len,
			   const uint8_t expect[32], const char *entry_sym,
			   struct llext **ext, void **entry);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_ */
