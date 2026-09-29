/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Per-application sandboxed files. The platform owns one filesystem mount and
 * confines each application to its own subtree, "/lfs/apps/<name>". An
 * application names a file relative to that root and cannot leave it: an
 * absolute path, or one holding a ".." component, is refused before the
 * filesystem is touched.
 *
 * The open file table is shared, so every handle records the application that
 * owns it. A handle is therefore meaningful only to its owner and one
 * application cannot reach another's file by guessing a small integer.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/internal/syscall_handler.h>
#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#include "app_grants.h"

LOG_MODULE_DECLARE(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

/* The service does not own a mount. It works within whichever filesystem is
 * already mounted at its configured mount point - the registry's, or one the
 * application image mounts itself - so the per-application roots do not depend
 * on a particular storage backend.
 */
#define MOUNT_POINT  CONFIG_APP_PLATFORM_FS_MOUNT_POINT
#define OPEN_FILES   CONFIG_APP_PLATFORM_FS_OPEN_FILES
#define PATH_MAX_LEN CONFIG_APP_PLATFORM_FS_PATH_MAX

BUILD_ASSERT(CONFIG_PRIVILEGED_STACK_SIZE >= 2048,
	     "a file call runs the filesystem on the privileged stack");

static struct fs_file_t files[OPEN_FILES];
static int file_owner[OPEN_FILES];
static bool file_used[OPEN_FILES];
static SVC_LOCK_DEFINE(fs_lock);

/* The application's name, which names its root directory. Only names that are
 * a plain directory component are accepted.
 */
static int root_name(int id, char *out)
{
	struct app_platform_status st;

	if (app_platform_status(id, &st) != 0 || st.name == NULL || st.name[0] == '\0' ||
	    st.name[0] == '.') {
		return -EINVAL;
	}
	for (size_t i = 0; st.name[i] != '\0'; i++) {
		char c = st.name[i];

		if (i + 1U >= APP_PLATFORM_NAME_LEN ||
		    !(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) {
			return -EINVAL;
		}
		out[i] = c;
		out[i + 1U] = '\0';
	}
	return 0;
}

/* Build the absolute path of a file inside the calling application's own root,
 * refusing anything that would leave it.
 */
static int confine_path(const char *name, const char *rel, char *out, size_t out_sz)
{
	if (rel == NULL || rel[0] == '\0') {
		return -EINVAL;
	}
	if (rel[0] == '/') {
		return -EACCES;
	}

	for (const char *p = rel; *p != '\0'; p++) {
		if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0') &&
		    (p == rel || p[-1] == '/')) {
			return -EACCES;
		}
	}

	int n = snprintk(out, out_sz, MOUNT_POINT "/apps/%s/%s", name, rel);

	if (n < 0 || (size_t)n >= out_sz) {
		return -ENAMETOOLONG;
	}
	return 0;
}

/* The mount is made by the system, not here. Opening a file creates the
 * application's directory on first use.
 */
static int fs_prepare(const char *name)
{
	char dir[PATH_MAX_LEN];
	int n;

	if (fs_mkdir(MOUNT_POINT "/apps") == -ENOENT) {
		return -ENODEV;
	}
	n = snprintk(dir, sizeof(dir), MOUNT_POINT "/apps/%s", name);
	if (n < 0 || (size_t)n >= sizeof(dir)) {
		return -ENAMETOOLONG;
	}

	int rc = fs_mkdir(dir);

	return (rc == 0 || rc == -EEXIST) ? 0 : rc;
}

/* Close every file a reclaimed application left open, so a recycled slot never
 * inherits a handle.
 */
void app_platform_clear_fs_handles(int id)
{
	if (app_service_lock(&fs_lock, K_FOREVER) != 0) {
		return;
	}
	for (int i = 0; i < OPEN_FILES; i++) {
		if (file_used[i] && file_owner[i] == id) {
			fs_close(&files[i]);
			file_used[i] = false;
			file_owner[i] = -1;
		}
	}
	app_service_unlock(&fs_lock);
}

int z_impl_svc_file_open(const char *path, int create)
{
	if (!app_has_cap(APP_CAP_FS)) {
		return -EPERM;
	}

	int id = app_platform_id_current();

	if (id < 0) {
		return -EPERM;
	}

	char name[APP_PLATFORM_NAME_LEN];
	char abs[PATH_MAX_LEN];
	int rc = root_name(id, name);

	if (rc == 0) {
		rc = confine_path(name, path, abs, sizeof(abs));
	}

	if (rc != 0) {
		return rc;
	}

	rc = app_service_lock(&fs_lock, K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	rc = fs_prepare(name);
	if (rc != 0) {
		app_service_unlock(&fs_lock);
		return rc;
	}

	int h = -1;

	for (int i = 0; i < OPEN_FILES; i++) {
		if (!file_used[i]) {
			h = i;
			break;
		}
	}
	if (h < 0) {
		app_service_unlock(&fs_lock);
		return -EMFILE;
	}

	fs_file_t_init(&files[h]);
	rc = fs_open(&files[h], abs, FS_O_RDWR | (create != 0 ? FS_O_CREATE : 0));
	if (rc != 0) {
		app_service_unlock(&fs_lock);
		return rc;
	}
	file_used[h] = true;
	file_owner[h] = id;
	app_service_unlock(&fs_lock);
	return h;
}

/* Resolve a handle to the caller's own open file, refusing another
 * application's handle and any handle that is not open.
 */
static int owned_file(int handle, struct fs_file_t **out)
{
	int id = app_platform_id_current();

	if (id < 0) {
		return -EPERM;
	}
	if (handle < 0 || handle >= OPEN_FILES || !file_used[handle]) {
		return -EBADF;
	}
	if (file_owner[handle] != id) {
		return -EACCES;
	}
	*out = &files[handle];
	return 0;
}

int z_impl_svc_file_write(int handle, const void *data, size_t len)
{
	if (!app_has_cap(APP_CAP_FS)) {
		return -EPERM;
	}
	if (len > CONFIG_APP_PLATFORM_FS_XFER_MAX) {
		return -EINVAL;
	}

	struct fs_file_t *f;
	int rc;

	rc = app_service_lock(&fs_lock, K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	rc = owned_file(handle, &f);
	if (rc == 0) {
		rc = (int)fs_write(f, data, len);
	}
	app_service_unlock(&fs_lock);
	return rc;
}

int z_impl_svc_file_read(int handle, void *buf, size_t len)
{
	if (!app_has_cap(APP_CAP_FS)) {
		return -EPERM;
	}
	if (len > CONFIG_APP_PLATFORM_FS_XFER_MAX) {
		return -EINVAL;
	}

	struct fs_file_t *f;
	int rc;

	rc = app_service_lock(&fs_lock, K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	rc = owned_file(handle, &f);
	if (rc == 0) {
		rc = (int)fs_read(f, buf, len);
	}
	app_service_unlock(&fs_lock);
	return rc;
}

int z_impl_svc_file_seek(int handle, int32_t off, int whence)
{
	if (!app_has_cap(APP_CAP_FS)) {
		return -EPERM;
	}

	struct fs_file_t *f;
	int rc;

	rc = app_service_lock(&fs_lock, K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	rc = owned_file(handle, &f);
	if (rc == 0) {
		rc = fs_seek(f, off, whence);
	}
	app_service_unlock(&fs_lock);
	return rc;
}

int z_impl_svc_file_close(int handle)
{
	if (!app_has_cap(APP_CAP_FS)) {
		return -EPERM;
	}

	struct fs_file_t *f;
	int rc;

	rc = app_service_lock(&fs_lock, K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	rc = owned_file(handle, &f);
	if (rc == 0) {
		fs_close(f);
		file_used[handle] = false;
		file_owner[handle] = -1;
	}
	app_service_unlock(&fs_lock);
	return rc;
}

#ifdef CONFIG_USERSPACE
static inline int z_vrfy_svc_file_open(const char *path, int create)
{
	char copy[PATH_MAX_LEN];

	if (k_usermode_string_copy(copy, (char *)path, sizeof(copy)) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_file_open(copy, create);
}
#include <zephyr/syscalls/svc_file_open_mrsh.c>

/* Bounce through a supervisor buffer rather than handing the filesystem a user
 * pointer, matching the other transfer services.
 */
static inline int z_vrfy_svc_file_write(int handle, const void *data, size_t len)
{
	char buf[CONFIG_APP_PLATFORM_FS_XFER_MAX];

	if (len > sizeof(buf)) {
		return -EINVAL;
	}
	if (k_usermode_from_copy(buf, (void *)data, len) != 0) {
		return -EFAULT;
	}
	return z_impl_svc_file_write(handle, buf, len);
}
#include <zephyr/syscalls/svc_file_write_mrsh.c>

static inline int z_vrfy_svc_file_read(int handle, void *buf, size_t len)
{
	char tmp[CONFIG_APP_PLATFORM_FS_XFER_MAX];

	if (len > sizeof(tmp)) {
		return -EINVAL;
	}

	int rc = z_impl_svc_file_read(handle, tmp, len);

	if (rc > 0 && k_usermode_to_copy(buf, tmp, rc) != 0) {
		return -EFAULT;
	}
	return rc;
}
#include <zephyr/syscalls/svc_file_read_mrsh.c>

static inline int z_vrfy_svc_file_seek(int handle, int32_t off, int whence)
{
	return z_impl_svc_file_seek(handle, off, whence);
}
#include <zephyr/syscalls/svc_file_seek_mrsh.c>

static inline int z_vrfy_svc_file_close(int handle)
{
	return z_impl_svc_file_close(handle);
}
#include <zephyr/syscalls/svc_file_close_mrsh.c>
#endif /* CONFIG_USERSPACE */
