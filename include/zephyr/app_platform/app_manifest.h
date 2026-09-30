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
 * The manifest also carries the major version of the application interface the
 * module was built against, @ref app_manifest.abi_major, and a module is loaded
 * only by a firmware exporting the same major version. Likewise @ref
 * app_manifest.target, a fingerprint of the architecture, SoC and compiler
 * options of the firmware build, must equal the firmware's. The heap size and
 * the capabilities the application requests and requires are part of the
 * signed body, so they are decided by the signer, not by whoever launches the
 * module.
 *
 * The manifest starts with a fixed header, the magic, the length of the signed
 * manifest and its format. The signature covers exactly @ref
 * app_manifest.hdr_len bytes. A firmware accepts a manifest at least as long
 * as the fields it knows, up to @ref APP_MANIFEST_MAX_LEN, and ignores the
 * bytes past them, so a later format can append fields without breaking an
 * older firmware. Fields are appended only; a change to an existing field takes
 * a new magic. A manifest of the previous fixed-layout format is refused and
 * the application has to be installed again.
 *
 * All fields have explicit widths, are little-endian and are packed without
 * padding, so the bytes signed on the build host are the bytes verified on the
 * device regardless of build configuration.
 */

/** Fixed name field width, independent of the registry's runtime maximum. */
#define APP_MANIFEST_NAME_MAX 32

/** Manifest magic: 'A' 'M' 'F' '4', little-endian. */
#define APP_MANIFEST_MAGIC 0x34464d41U

/** Manifest format this firmware writes and the oldest it accepts. */
#define APP_MANIFEST_FORMAT 4U

/** Largest signed manifest this firmware accepts. */
#define APP_MANIFEST_MAX_LEN 256U

/** Interface build identifier length. */
#define APP_MANIFEST_ABI_ID_LEN 16

/** Raw ECDSA P-256 signature length (r || s). */
#define APP_MANIFEST_SIG_LEN 64

/** The signed manifest body, as far as this firmware knows it. The signature is
 * carried separately.
 */
struct app_manifest {
	uint32_t magic;                           /**< APP_MANIFEST_MAGIC */
	uint16_t hdr_len;                         /**< signed manifest length in bytes */
	uint16_t format;                          /**< APP_MANIFEST_FORMAT or later */
	uint32_t version;                         /**< monotonic app version */
	uint32_t module_len;                      /**< expected module size */
	char     name[APP_MANIFEST_NAME_MAX];     /**< authenticated app name */
	uint8_t  module_sha256[32];               /**< SHA-256 of the module */
	uint8_t  abi_id[APP_MANIFEST_ABI_ID_LEN]; /**< interface build identifier, not checked */
	uint32_t target;                          /**< target the module was built for */
	uint16_t abi_major;                       /**< application interface major version */
	uint16_t reserved0;                       /**< zero */
	uint32_t heap_size;                       /**< per-application heap bytes, 0 for none */
	uint32_t key_id;                          /**< signing key, 0 for the default key */
	uint64_t requested_caps;                  /**< capabilities the app asks for */
	uint64_t required_caps;                   /**< capabilities the app cannot run without */
	uint32_t flags;                           /**< zero, no flag is defined */
	uint8_t  reserved[28];                    /**< zero */
} __packed;

BUILD_ASSERT(sizeof(struct app_manifest) == 160, "app_manifest must be packed with no padding");
BUILD_ASSERT(sizeof(struct app_manifest) <= APP_MANIFEST_MAX_LEN);

/** Room for a manifest of any length this firmware accepts. */
union app_manifest_buf {
	struct app_manifest mf;             /**< the fields this firmware knows */
	uint8_t raw[APP_MANIFEST_MAX_LEN];  /**< the signed bytes */
};

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_MANIFEST_H_ */
