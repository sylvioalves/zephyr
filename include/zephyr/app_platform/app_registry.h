/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_APP_PLATFORM_REGISTRY_H_
#define ZEPHYR_INCLUDE_APP_PLATFORM_REGISTRY_H_

#include <zephyr/kernel.h>
#include <zephyr/app_platform/app_manifest.h>

/**
 * @brief Application registry
 * @ingroup app_platform
 * @{
 *
 * The installed-application registry, persisted to a filesystem. Installing an
 * application records its metadata so the platform knows, across reboots, what
 * is installed and should be launched at boot - the difference between "an
 * application ran once" and "an application is installed on the device".
 */

#define APP_REG_MAX  CONFIG_APP_PLATFORM_REGISTRY_MAX_APPS

/* A record holds a shorter name than a manifest carries
 * (@ref APP_MANIFEST_NAME_MAX). Installing an application whose authenticated
 * name does not fit is refused with -ENAMETOOLONG rather than truncated, so
 * two names sharing a prefix can never share one record.
 */
#define APP_NAME_MAX CONFIG_APP_PLATFORM_REGISTRY_NAME_MAX

/** A persisted record of an installed application. */
struct app_record {
	char     name[APP_NAME_MAX];  /**< application name */
	uint8_t  enabled;             /**< present and runnable */
	uint8_t  autostart;           /**< launch at boot */
	uint32_t install_seq;         /**< bumped on each install */
	uint32_t version;             /**< signed image version, monotonic */
	uint8_t  bound;               /**< @p module_sha256 names the committed image */
	uint8_t  module_sha256[32];   /**< SHA-256 of the committed module */
};

/**
 * @brief Mount the storage and load the registry. Call once at boot.
 *
 * An absent, unreadable or inconsistent registry is replaced with an empty one
 * rather than treated as an error, so a first boot and a corrupted file behave
 * alike.
 *
 * @retval 0 Success.
 * @retval -errno The storage could not be mounted, or the rebuilt registry
 *         could not be written back.
 */
int app_registry_load(void);

/**
 * @brief Install (or update) an application record and persist it.
 *
 * Equivalent to @ref app_registry_install_versioned with version zero, for a
 * caller that does not track versions.
 *
 * @param name Application name, shorter than @ref APP_NAME_MAX.
 * @param autostart Whether the platform launches it at boot.
 *
 * @retval 0 Success.
 * @retval -ENAMETOOLONG @p name does not fit a record.
 * @retval -ENOSPC The registry already holds @ref APP_REG_MAX records.
 * @retval -errno The registry could not be written back.
 */
int app_registry_install(const char *name, bool autostart);

/**
 * @brief Install (or update) an application, rejecting a version downgrade.
 *
 * Records @p version and refuses to overwrite an existing record with a lower
 * version, so a signed-but-old image cannot be replayed onto a device that has
 * already accepted a newer one. Equal versions are accepted (re-install of the
 * same image). Unlike @ref app_registry_commit it knows nothing of the new
 * image, so it makes no exception for an installed image this firmware cannot
 * load.
 *
 * @retval 0 on success.
 * @retval -EACCES if @p version is older than the installed record's version.
 * @retval other negative errno on failure.
 */
int app_registry_install_versioned(const char *name, bool autostart,
				   uint32_t version);

/**
 * @brief Number of installed applications.
 *
 * @return The record count, always zero or more.
 */
int app_registry_count(void);

/**
 * @brief Fetch an installed application record by index.
 *
 * @param idx Index below @ref app_registry_count.
 * @param out Receives a copy of the record.
 *
 * @retval 0 Success.
 * @retval -EINVAL @p idx is out of range, or @p out is NULL.
 */
int app_registry_get(int idx, struct app_record *out);

/**
 * @brief Launch callback for @ref app_registry_autostart_all.
 *
 * Called once per enabled autostart record. The callback owns the load and
 * launch policy - which key to verify against, which capabilities to grant -
 * because that is application, not platform, policy. The CPU mask, and the
 * configure hook or stopped launch that gives the application its grants, are
 * set by the callback too, through @ref app_launch_opts.
 *
 * @param rec  The installed-application record to launch.
 * @param user Opaque pointer passed through from the caller.
 * @return The application id on success, negative errno on failure.
 */
typedef int (*app_registry_launch_t)(const struct app_record *rec, void *user);

/**
 * @brief Launch every installed application flagged for autostart.
 *
 * Walks the registry in install order and invokes @p launch for each enabled
 * record whose autostart flag is set. A record that fails to launch is logged
 * as an error with its name and reason, its result is recorded for
 * @ref app_registry_autostart_result, and it is skipped so one bad application
 * does not stop the rest from starting.
 *
 * @param launch Callback that loads and launches one record.
 * @param user   Opaque pointer handed to @p launch.
 * @retval >=0 the number of applications launched.
 * @retval -EINVAL if @p launch is NULL.
 */
int app_registry_autostart_all(app_registry_launch_t launch, void *user);

/**
 * @brief Result of the last autostart launch of an installed application.
 *
 * The result is kept in RAM only, so the on-flash registry format is unchanged.
 * It is cleared at boot by @ref app_registry_load and when the application is
 * installed or committed again.
 *
 * @param name Application name.
 * @param[out] err Receives 0 when the last autostart launched the application or
 *             none was attempted, or the negative errno its launch returned, for
 *             example -EPERM when a required capability is not allowed, -ENOEXEC
 *             when it was built for another interface major version or target and
 *             -ENOTSUP when its manifest format is retired.
 *
 * @retval 0 Success.
 * @retval -ENOENT No application with this name is installed, or @p err is NULL.
 */
int app_registry_autostart_result(const char *name, int *err);

/**
 * @brief Remove an installed application and persist the registry.
 *
 * @param name Name of the record to remove.
 *
 * @retval 0 Success.
 * @retval -ENOENT No record carries that name.
 * @retval -errno The registry could not be written back.
 */
int app_registry_uninstall(const char *name);

/**
 * @brief Persist an application's binary image to storage.
 *
 * Stores the verified module bytes so the application can be reloaded after a
 * reboot, keyed by name. Pair this with @ref app_registry_install to record
 * the metadata.
 *
 * @param name Application name, used as the storage key.
 * @param buf  Module bytes to persist.
 * @param len  Number of bytes.
 *
 * @retval 0 Success.
 * @retval -EIO The image was written only in part.
 * @retval -errno The file could not be created or written.
 */
int app_registry_store_blob(const char *name, const uint8_t *buf, size_t len);

/**
 * @brief Read back an application's persisted binary image.
 *
 * @param name     Application name.
 * @param buf      Destination buffer.
 * @param cap      Size of @p buf.
 * @param out_len  Set to the number of bytes read.
 * @return 0 on success, negative errno on failure (e.g. -ENOENT if not stored).
 */
int app_registry_load_blob(const char *name, uint8_t *buf, size_t cap,
			   size_t *out_len);

/**
 * @brief Build the on-flash path of an application's binary.
 *
 * Yields "<mount>/<name>.llext" so a caller can stream-load the module from the
 * file without holding it in memory.
 *
 * @param name Application name.
 * @param out  Destination buffer for the path.
 * @param cap  Size of @p out.
 * @return 0 on success, -ENAMETOOLONG if the path does not fit.
 */
int app_registry_blob_path(const char *name, char *out, size_t cap);

/**
 * @brief Persist an application's signed manifest and signature.
 *
 * The manifest binds the application's name and version to the module bytes,
 * and is stored so both can be re-verified from flash after a reboot.
 *
 * @param name    Application name (the storage key).
 * @param mf      Signed manifest body.
 * @param mf_len  Length of @p mf, at most @ref APP_MANIFEST_MAX_LEN.
 * @param sig     Signature over @p mf.
 * @param sig_len Length of @p sig (must be @ref APP_MANIFEST_SIG_LEN).
 * @return 0 on success, negative errno on failure.
 */
int app_registry_store_manifest(const char *name, const struct app_manifest *mf, size_t mf_len,
				const uint8_t *sig, size_t sig_len);

/**
 * @brief Read back an application's persisted manifest and signature.
 *
 * The stored manifest is returned as it was written, whatever its length, and
 * the part of @p mf past it is zeroed. The loader decides whether it is
 * accepted.
 *
 * @param name    Application name.
 * @param[out] mf Receives the manifest body.
 * @param[out] mf_len Receives the length of the manifest body.
 * @param[out] sig Receives the signature (@ref APP_MANIFEST_SIG_LEN bytes).
 * @param sig_cap Size of @p sig (must be at least @ref APP_MANIFEST_SIG_LEN).
 * @return 0 on success, negative errno on failure (e.g. -ENOENT if not stored,
 *         -EFBIG if the manifest is longer than @ref APP_MANIFEST_MAX_LEN).
 */
int app_registry_load_manifest(const char *name, union app_manifest_buf *mf, size_t *mf_len,
			       uint8_t *sig, size_t sig_cap);

/**
 * @brief Build the on-flash path of an application's staged binary.
 *
 * A download is written to the staged path, verified from there and only
 * then moved over the installed image by @ref app_registry_commit, so a
 * transfer that fails or does not verify never replaces a working install.
 *
 * @param name Application name.
 * @param out  Destination buffer for the path.
 * @param cap  Size of @p out.
 *
 * @retval 0 Success.
 * @retval -ENAMETOOLONG The path does not fit @p out.
 */
int app_registry_staged_blob_path(const char *name, char *out, size_t cap);

/**
 * @brief Build the on-flash path of an application's staged manifest.
 *
 * The file holds the manifest body followed by its signature, as written by
 * @ref app_registry_store_staged_manifest.
 *
 * @param name Application name.
 * @param out  Destination buffer for the path.
 * @param cap  Size of @p out.
 *
 * @retval 0 Success.
 * @retval -ENAMETOOLONG The path does not fit @p out.
 */
int app_registry_staged_manifest_path(const char *name, char *out, size_t cap);

/**
 * @brief Persist a downloaded manifest and signature to the staged path.
 *
 * @param name    Application name (the storage key).
 * @param mf      Signed manifest body.
 * @param mf_len  Length of @p mf, at most @ref APP_MANIFEST_MAX_LEN.
 * @param sig     Signature over @p mf.
 * @param sig_len Length of @p sig (must be @ref APP_MANIFEST_SIG_LEN).
 *
 * @retval 0 Success.
 * @retval -EINVAL @p sig_len is not @ref APP_MANIFEST_SIG_LEN or @p mf_len is
 *         zero or above @ref APP_MANIFEST_MAX_LEN.
 * @retval -ENAMETOOLONG The path does not fit.
 * @retval -EIO The manifest was written only in part.
 * @retval -errno The file could not be created.
 */
int app_registry_store_staged_manifest(const char *name, const struct app_manifest *mf,
				       size_t mf_len, const uint8_t *sig, size_t sig_len);

/**
 * @brief Read back a staged manifest and signature.
 *
 * @param name    Application name.
 * @param[out] mf Receives the manifest body, zeroed past its length.
 * @param[out] mf_len Receives the length of the manifest body.
 * @param[out] sig Receives the signature (@ref APP_MANIFEST_SIG_LEN bytes).
 * @param sig_cap Size of @p sig.
 *
 * @retval 0 Success.
 * @retval -EINVAL @p sig_cap is smaller than @ref APP_MANIFEST_SIG_LEN.
 * @retval -ENAMETOOLONG The path does not fit.
 * @retval -EIO The file holds no manifest before the signature.
 * @retval -EFBIG The manifest is longer than @ref APP_MANIFEST_MAX_LEN.
 * @retval -errno The file could not be opened.
 */
int app_registry_load_staged_manifest(const char *name, union app_manifest_buf *mf,
				      size_t *mf_len, uint8_t *sig, size_t sig_cap);

/**
 * @brief Install a verified staged image in place of the current one.
 *
 * The caller has verified the staged manifest and module and passes the
 * verified manifest. The version is checked against the installed record
 * before anything is moved, so a downgrade leaves the installed image and
 * record untouched and the staged files are discarded. On success the
 * staged files replace the installed ones and the record is bound to
 * @p mf's module hash, which @ref app_registry_check enforces at boot.
 *
 * A lower version is installed, and logged, only when this firmware cannot
 * load the installed image, because its manifest has a retired format or names
 * another interface major version or target, and @p mf names this firmware's.
 * After a firmware rollback to an older interface major version this lets the
 * application be brought back to a version built for it.
 *
 * @param name      Application name.
 * @param autostart Whether the platform launches it at boot.
 * @param mf        The verified manifest of the staged image.
 *
 * @retval 0 Success.
 * @retval -EINVAL @p mf names a different application.
 * @retval -ENAMETOOLONG @p name does not fit a record.
 * @retval -EACCES @p mf is older than an installed version this firmware loads.
 * @retval -ENOSPC The registry already holds @ref APP_REG_MAX records.
 * @retval -errno The staged files could not be moved, or the registry
 *         could not be written back.
 */
int app_registry_commit(const char *name, bool autostart, const struct app_manifest *mf);

/**
 * @brief Remove an application's staged files, if any.
 *
 * @param name Application name.
 */
void app_registry_discard(const char *name);

/**
 * @brief Check a verified manifest against its installed record.
 *
 * Call after verifying an installed image and before launching it. The
 * installed files can be replaced by anything with write access to the
 * storage, including an older image that still carries a valid signature;
 * this refuses any manifest other than the one that was committed.
 *
 * @param rec The installed record.
 * @param mf  The manifest just verified from storage.
 *
 * @retval 0 @p mf is the committed image.
 * @retval -EINVAL @p mf names a different application.
 * @retval -EACCES @p mf is older than the recorded version.
 * @retval -EBADMSG @p mf describes a module other than the committed one.
 */
int app_registry_check(const struct app_record *rec, const struct app_manifest *mf);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_REGISTRY_H_ */
