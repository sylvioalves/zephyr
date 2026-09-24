/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_MANIFEST_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_MANIFEST_H_

#include <zephyr/kernel.h>

/**
 * @brief Signed application manifest
 * @ingroup app_platform
 * @{
 *
 * A manifest binds an application's identity - its name and version - to the
 * exact module bytes, and is what the signature actually covers. Verification
 * is two steps: the ECDSA P-256 signature authenticates the manifest, then the
 * module is accepted only if its SHA-256 matches @ref app_manifest.module_sha256.
 * So the name and version are authenticated, not asserted by the caller, and a
 * valid signature cannot be moved onto different bytes or relabelled.
 *
 * The manifest also carries the ABI identifier of the firmware the module was
 * built against, derived at build time from the syscall numbering and the
 * application interface headers. A module is loaded only by a firmware with the
 * same identifier, since a module built for another numbering would invoke the
 * wrong system calls.
 *
 * The layout is a fixed binary struct with explicit-width fields and no
 * padding, so the bytes signed on the build host are the bytes verified on the
 * device regardless of build configuration.
 */

/** Fixed name field width, independent of the registry's runtime maximum. */
#define APP_MANIFEST_NAME_MAX 32

/** Manifest magic: 'A' 'M' 'F' '2', little-endian. */
#define APP_MANIFEST_MAGIC 0x32464d41U

/** ABI identifier length. */
#define APP_MANIFEST_ABI_ID_LEN 16

/** Raw ECDSA P-256 signature length (r || s). */
#define APP_MANIFEST_SIG_LEN 64

/** The signed manifest body. The signature is carried separately. */
struct app_manifest {
	uint32_t magic;                          /**< APP_MANIFEST_MAGIC */
	uint32_t version;                        /**< monotonic app version */
	uint32_t module_len;                     /**< expected module size */
	char     name[APP_MANIFEST_NAME_MAX];    /**< authenticated app name */
	uint8_t  module_sha256[32];              /**< SHA-256 of the module */
	uint8_t  abi_id[APP_MANIFEST_ABI_ID_LEN]; /**< ABI the module was built for */
} __packed;

BUILD_ASSERT(sizeof(struct app_manifest) ==
	     4 + 4 + 4 + APP_MANIFEST_NAME_MAX + 32 + APP_MANIFEST_ABI_ID_LEN,
	     "app_manifest must be tightly packed with no padding");

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_MANIFEST_H_ */
