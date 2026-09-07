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
};

/** Mount the storage and load the registry. Call once at boot. */
int app_registry_load(void);

/** Install (or update) an application record and persist it. */
int app_registry_install(const char *name, bool autostart);

/**
 * @brief Install (or update) an application, rejecting a version downgrade.
 *
 * Records @p version and refuses to overwrite an existing record with a lower
 * version, so a signed-but-old image cannot be replayed onto a device that has
 * already accepted a newer one. Equal versions are accepted (re-install of the
 * same image).
 *
 * @retval 0 on success.
 * @retval -EACCES if @p version is older than the installed record's version.
 * @retval other negative errno on failure.
 */
int app_registry_install_versioned(const char *name, bool autostart,
				   uint32_t version);

/** Number of installed applications. */
int app_registry_count(void);

/** Fetch an installed application record by index. */
int app_registry_get(int idx, struct app_record *out);

/**
 * @brief Launch callback for @ref app_registry_autostart_all.
 *
 * Called once per enabled autostart record. The callback owns the load and
 * launch policy - which key to verify against, which capabilities to grant -
 * because that is application, not platform, policy.
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
 * record whose autostart flag is set. A record that fails to launch is skipped
 * so one bad application does not stop the rest from starting.
 *
 * @param launch Callback that loads and launches one record.
 * @param user   Opaque pointer handed to @p launch.
 * @retval >=0 the number of applications launched.
 * @retval -EINVAL if @p launch is NULL.
 */
int app_registry_autostart_all(app_registry_launch_t launch, void *user);

/** Remove an installed application and persist. */
int app_registry_uninstall(const char *name);

/**
 * @brief Persist an application's binary image to storage.
 *
 * Stores the verified module bytes so the application can be reloaded after a
 * reboot, keyed by name. Pair this with @ref app_registry_install to record
 * the metadata.
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
 * @param sig     Signature over @p mf.
 * @param sig_len Length of @p sig (must be @ref APP_MANIFEST_SIG_LEN).
 * @return 0 on success, negative errno on failure.
 */
int app_registry_store_manifest(const char *name, const struct app_manifest *mf,
				const uint8_t *sig, size_t sig_len);

/**
 * @brief Read back an application's persisted manifest and signature.
 *
 * @param name    Application name.
 * @param[out] mf Receives the manifest body.
 * @param[out] sig Receives the signature (@ref APP_MANIFEST_SIG_LEN bytes).
 * @param sig_cap Size of @p sig (must be at least @ref APP_MANIFEST_SIG_LEN).
 * @return 0 on success, negative errno on failure (e.g. -ENOENT if not stored).
 */
int app_registry_load_manifest(const char *name, struct app_manifest *mf,
			       uint8_t *sig, size_t sig_cap);

/** @} */

#endif /* ZEPHYR_INCLUDE_APP_PLATFORM_REGISTRY_H_ */
