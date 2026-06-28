/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_

#include <zephyr/llext/llext.h>
#include <zephyr/app_platform/app_manifest.h>

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
 * This proves integrity only: it detects corruption, not forgery. To accept a
 * module from an untrusted source (a download), use @ref
 * app_loader_verify_load_signed, which proves the module was signed by the
 * holder of a trusted private key.
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
 * @retval -E2BIG if the module fails a structural bounds check.
 * @retval other negative errno from crypto or llext on failure.
 */
int app_loader_verify_load(const char *name, const uint8_t *buf, size_t len,
			   const uint8_t expect[32], const char *entry_sym,
			   struct llext **ext, void **entry);

/**
 * @brief Verify a module's signature and load it as an llext.
 *
 * Authenticates a module before loading: the ECDSA P-256 signature over the
 * SHA-256 of the module is checked against @p pubkey, which is the trusted
 * public key compiled into the firmware. A module that was not signed by the
 * matching private key - including one whose bytes were altered after signing -
 * is rejected and never linked. This is the install path an untrusted,
 * network-downloaded module must follow.
 *
 * The module is also structurally bounds-checked before any parsing, so a
 * malformed image is rejected before it can exercise the loader.
 *
 * @param name      Name for the loaded extension.
 * @param buf       Module bytes.
 * @param len       Module length.
 * @param sig       ECDSA P-256 signature (raw r||s, 64 bytes).
 * @param sig_len   Length of @p sig.
 * @param pubkey    Trusted public key, SEC1 uncompressed point (65 bytes,
 *                  0x04 || X || Y).
 * @param pubkey_len Length of @p pubkey.
 * @param entry_sym Entry symbol name to resolve.
 * @param[out] ext  Receives the loaded extension on success.
 * @param[out] entry Receives the resolved entry pointer on success.
 *
 * @retval 0 on success.
 * @retval -EPERM if the signature does not verify.
 * @retval -ENOENT if the entry symbol is not found.
 * @retval -E2BIG if the module fails a structural bounds check.
 * @retval other negative errno from crypto or llext on failure.
 */
int app_loader_verify_load_signed(const char *name, const uint8_t *buf,
				  size_t len, const uint8_t *sig, size_t sig_len,
				  const uint8_t *pubkey, size_t pubkey_len,
				  const char *entry_sym, struct llext **ext,
				  void **entry);

/**
 * @brief Verify a signed manifest, then load the module it binds.
 *
 * The strongest path: the ECDSA P-256 signature is checked over the @p mf
 * manifest, so the name and version the manifest carries are authenticated -
 * not asserted by the caller. The module is then accepted only if its length
 * and SHA-256 match the signed manifest, so a valid signature cannot be moved
 * onto different bytes or relabelled. The module is also bounds-checked before
 * any parsing. The loaded extension takes its name from the manifest.
 *
 * @param mf        Manifest body (the bytes the signature covers).
 * @param sig       ECDSA P-256 signature over @p mf (raw r||s, 64 bytes).
 * @param sig_len   Length of @p sig.
 * @param buf       Module bytes.
 * @param len       Module length.
 * @param pubkey    Trusted public key, SEC1 uncompressed point (65 bytes).
 * @param pubkey_len Length of @p pubkey.
 * @param entry_sym Entry symbol name to resolve.
 * @param[out] ext  Receives the loaded extension on success.
 * @param[out] entry Receives the resolved entry pointer on success.
 *
 * @retval 0 on success.
 * @retval -EPERM if the signature, manifest, or module binding does not check.
 * @retval -ENOENT if the entry symbol is not found.
 * @retval -E2BIG if the module fails a structural bounds check.
 * @retval other negative errno from crypto or llext on failure.
 */
int app_loader_verify_load_manifest(const struct app_manifest *mf,
				    const uint8_t *sig, size_t sig_len,
				    const uint8_t *buf, size_t len,
				    const uint8_t *pubkey, size_t pubkey_len,
				    const char *entry_sym, struct llext **ext,
				    void **entry);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_ */
