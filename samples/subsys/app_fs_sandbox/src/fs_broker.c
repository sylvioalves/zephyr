/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fs_broker.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>
#include <errno.h>

#define MOUNT_POINT "/lfs"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_cfg,
	.storage_dev = (void *)PARTITION_ID(storage_partition),
	.mnt_point = MOUNT_POINT,
};

/* Per-app namespace registry. */
static k_tid_t app_tids[FS_MAX_APPS];
static int app_n;

/* Open-file table (handles are indices). */
static struct fs_file_t files[FS_OPEN_FILES];
static bool file_used[FS_OPEN_FILES];

static int app_id_of_current(void)
{
	k_tid_t cur = k_current_get();

	for (int i = 0; i < FS_MAX_APPS; i++) {
		if (app_tids[i] == cur) {
			return i;
		}
	}
	return -1;
}

/*
 * The security-critical function: build the absolute path for the calling app
 * inside its own namespace, rejecting any attempt to escape it. A safe relative
 * path maps to "/lfs/apps/<id>/<rel>". An absolute path, or one containing a
 * ".." component, is denied.
 */
static int confine_path(int app_id, const char *rel, char *out, size_t out_sz)
{
	if (rel == NULL || rel[0] == '\0') {
		return -EINVAL;
	}
	if (rel[0] == '/') {
		printk("    [fs-broker] DENIED absolute path '%s' (escape)\n", rel);
		return -EACCES;
	}

	/* Reject any ".." path component. */
	for (const char *p = rel; *p != '\0'; p++) {
		if (p[0] == '.' && p[1] == '.' &&
		    (p[2] == '/' || p[2] == '\0') &&
		    (p == rel || p[-1] == '/')) {
			printk("    [fs-broker] DENIED path traversal '%s' (escape)\n",
			       rel);
			return -EACCES;
		}
	}

	int n = snprintk(out, out_sz, MOUNT_POINT "/apps/%d/%s", app_id, rel);

	if (n < 0 || (size_t)n >= out_sz) {
		return -ENAMETOOLONG;
	}
	return 0;
}

static int ensure_app_dir(int app_id)
{
	char dir[FS_PATH_MAX];

	(void)snprintk(dir, sizeof(dir), MOUNT_POINT "/apps");
	(void)fs_mkdir(dir);
	(void)snprintk(dir, sizeof(dir), MOUNT_POINT "/apps/%d", app_id);
	int rc = fs_mkdir(dir);

	return (rc == 0 || rc == -EEXIST) ? 0 : rc;
}

int fs_broker_init(void)
{
	int rc = fs_mount(&mnt);

	if (rc != 0 && rc != -EBUSY) {
		printk("fs-broker: mount failed: %d\n", rc);
		return rc;
	}
	(void)fs_mkdir(MOUNT_POINT "/apps");
	printk("fs-broker: filesystem mounted at %s\n", MOUNT_POINT);
	return 0;
}

int fs_broker_register(k_tid_t tid)
{
	if (app_n >= FS_MAX_APPS) {
		return -ENOSPC;
	}
	int id = app_n++;

	app_tids[id] = tid;
	ensure_app_dir(id);
	return id;
}

int fs_broker_open(const char *rel_path, bool create)
{
	int app_id = app_id_of_current();

	if (app_id < 0) {
		return -EPERM;
	}

	char path[FS_PATH_MAX];
	int rc = confine_path(app_id, rel_path, path, sizeof(path));

	if (rc != 0) {
		return rc;   /* escape denied here */
	}

	int h = -1;

	for (int i = 0; i < FS_OPEN_FILES; i++) {
		if (!file_used[i]) {
			h = i;
			break;
		}
	}
	if (h < 0) {
		return -EMFILE;
	}

	fs_file_t_init(&files[h]);
	fs_mode_t flags = FS_O_RDWR | (create ? FS_O_CREATE : 0);

	rc = fs_open(&files[h], path, flags);
	if (rc != 0) {
		return rc;
	}
	file_used[h] = true;
	printk("    [fs-broker] app %d opened '%s' -> %s (handle %d)\n",
	       app_id, rel_path, path, h);
	return h;
}

int fs_broker_write(int handle, const char *data, size_t len)
{
	if (handle < 0 || handle >= FS_OPEN_FILES || !file_used[handle]) {
		return -EBADF;
	}
	return (int)fs_write(&files[handle], data, len);
}

int fs_broker_read(int handle, char *buf, size_t len)
{
	if (handle < 0 || handle >= FS_OPEN_FILES || !file_used[handle]) {
		return -EBADF;
	}
	(void)fs_seek(&files[handle], 0, FS_SEEK_SET);
	return (int)fs_read(&files[handle], buf, len);
}

int fs_broker_close(int handle)
{
	if (handle < 0 || handle >= FS_OPEN_FILES || !file_used[handle]) {
		return -EBADF;
	}
	fs_close(&files[handle]);
	file_used[handle] = false;
	return 0;
}
