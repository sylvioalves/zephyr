/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_

#include <zephyr/llext/llext.h>
#include <zephyr/app_platform/app_caps.h>
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
 * app_loader_verify_load_manifest, which proves the module was signed by a
 * trusted publisher.
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
 * @retval -EACCES if the module imports a symbol modules may not use.
 * @retval -ENOEXEC if the module was built for another floating-point ABI or
 *         instruction set.
 * @retval -EALREADY if an extension with this name is already loaded.
 * @retval other negative errno from crypto or llext on failure.
 */
int app_loader_verify_load(const char *name, const uint8_t *buf, size_t len,
			   const uint8_t expect[32], const char *entry_sym,
			   struct llext **ext, void **entry);

/**
 * @brief Verify a signed manifest, then load the module it binds.
 *
 * The ECDSA P-256 signature over the @p mf manifest is checked with the public
 * key the manifest names by @c key_id in the trusted key table compiled into
 * the firmware, so the name and version the manifest carries are authenticated -
 * not asserted by the caller. The name must start with the prefix the key may
 * sign. The module is then accepted only if its length and SHA-256 match the
 * signed manifest, so a valid signature cannot be moved onto different bytes or
 * relabelled. The module is also bounds-checked before any parsing. The loaded
 * extension takes its name from the manifest.
 *
 * @param mf        Manifest body (the bytes the signature covers).
 * @param mf_len    Length of @p mf, which must equal its @c hdr_len.
 * @param sig       ECDSA P-256 signature over @p mf (raw r||s, 64 bytes).
 * @param sig_len   Length of @p sig.
 * @param buf       Module bytes.
 * @param len       Module length.
 * @param entry_sym Entry symbol name to resolve.
 * @param[out] ext  Receives the loaded extension on success.
 * @param[out] entry Receives the resolved entry pointer on success.
 *
 * @retval 0 on success.
 * @retval -EPERM if the signature or module binding does not check, or the key
 *         may not sign the application name.
 * @retval -ENXIO if the manifest names a key this firmware does not trust, or
 *         the development key while @kconfig{CONFIG_APP_PLATFORM_DEV_KEY} is
 *         disabled.
 * @retval -ECANCELED if the manifest names a revoked key.
 * @retval -EBADMSG if the manifest is malformed, its length is not accepted or
 *         it requires a capability it does not request.
 * @retval -ENOTSUP if the manifest has a retired format; reinstall the application.
 * @retval -ENOEXEC if the manifest names another interface major version, or the
 *         module was built for another floating-point ABI or instruction set.
 * @retval -ENOENT if the entry symbol is not found.
 * @retval -E2BIG if the module fails a structural bounds check.
 * @retval -EACCES if the module imports a symbol modules may not use.
 * @retval -EALREADY if an extension with this name is already loaded.
 * @retval other negative errno from crypto or llext on failure.
 */
int app_loader_verify_load_manifest(const struct app_manifest *mf, size_t mf_len,
				    const uint8_t *sig, size_t sig_len, const uint8_t *buf,
				    size_t len, const char *entry_sym, struct llext **ext,
				    void **entry);

/**
 * @brief Verify a signed manifest, then load the module from a file.
 *
 * Like @ref app_loader_verify_load_manifest, but the module comes from the
 * filesystem. With @kconfig{CONFIG_APP_PLATFORM_LOADER_FILE_IN_RAM} the file is
 * read once into a heap buffer that is verified and linked, and freed after
 * loading. Otherwise the file is hashed as a stream, linked from the file and
 * re-hashed afterwards, so no copy of the whole module is held in memory. The
 * manifest binds and authenticates the name, version and module hash.
 *
 * @param mf        Manifest body (the bytes the signature covers).
 * @param mf_len    Length of @p mf, which must equal its @c hdr_len.
 * @param sig       ECDSA P-256 signature over @p mf (raw r||s, 64 bytes).
 * @param sig_len   Length of @p sig.
 * @param path      Absolute filesystem path of the module.
 * @param entry_sym Entry symbol name to resolve.
 * @param[out] ext  Receives the loaded extension on success.
 * @param[out] entry Receives the resolved entry pointer on success.
 *
 * @retval 0 on success.
 * @retval -EPERM if the signature or module binding does not check, or the key
 *         may not sign the application name.
 * @retval -ENXIO if the manifest names a key this firmware does not trust, or
 *         the development key while @kconfig{CONFIG_APP_PLATFORM_DEV_KEY} is
 *         disabled.
 * @retval -ECANCELED if the manifest names a revoked key.
 * @retval -EBADMSG if the manifest is malformed, its length is not accepted or
 *         it requires a capability it does not request.
 * @retval -ENOTSUP if the manifest has a retired format; reinstall the application.
 * @retval -ENOEXEC if the manifest names another interface major version, or the
 *         module was built for another floating-point ABI or instruction set.
 * @retval -ENOENT if the entry symbol is not found.
 * @retval -E2BIG if the module fails a structural bounds check.
 * @retval -EACCES if the module imports a symbol modules may not use.
 * @retval -EALREADY if an extension with this name is already loaded.
 * @retval -EFBIG if the signed length is zero or above the module budget.
 * @retval -ENOMEM if the heap cannot hold the module copy.
 * @retval other negative errno from filesystem, crypto or llext on failure.
 */
int app_loader_verify_load_manifest_file(const struct app_manifest *mf, size_t mf_len,
					 const uint8_t *sig, size_t sig_len, const char *path,
					 const char *entry_sym, struct llext **ext, void **entry);

/**
 * @brief Report whether an extension was produced by a verifying loader.
 *
 * Only an extension that one of the @c app_loader_verify_* functions loaded and
 * verified is recorded as verified. The platform launch path consults this so
 * an extension that did not pass verification cannot be launched, even if a
 * caller obtained a @ref llext by other means.
 *
 * @param ext Extension to query.
 * @retval true if @p ext was verified by this loader.
 * @retval false otherwise.
 */
bool app_loader_is_verified(const struct llext *ext);

/**
 * @brief Heap size the signed manifest of a verified extension declares.
 *
 * The platform launch path gives the application a heap of this size. An
 * extension verified without a manifest has none.
 *
 * @param ext Extension to query.
 * @return The heap size in bytes, or 0 when @p ext declares no heap or is not
 *         verified.
 */
size_t app_loader_heap_size(const struct llext *ext);

/**
 * @brief Capabilities the signed manifest of a verified extension declares.
 *
 * The platform launch path grants the application the requested capabilities
 * that the launcher allows, and refuses the launch when a required one is not
 * allowed, see @ref app_platform_launch_llext.
 *
 * @param ext Extension to query.
 * @param[out] requested Receives the capabilities the application asks for.
 * @param[out] required Receives the capabilities it cannot run without.
 * @retval 0 on success.
 * @retval -ENOENT @p ext is not verified, or was verified without a manifest.
 */
int app_loader_manifest_caps(const struct llext *ext, app_caps_t *requested,
			     app_caps_t *required);

/**
 * @brief Drop an extension from the verified set.
 *
 * Call when unloading a verified extension so its handle does not linger as
 * verified for a future, unrelated allocation at the same address.
 *
 * @param ext Extension to forget.
 */
void app_loader_forget(const struct llext *ext);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_LOADER_H_ */
