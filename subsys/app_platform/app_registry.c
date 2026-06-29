/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The installed-application registry, persisted to a littlefs partition so the
 * set of installed applications survives reboots.
 */

#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_manifest.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_DECLARE(app_platform, CONFIG_APP_PLATFORM_LOG_LEVEL);

#define MOUNT_POINT CONFIG_APP_PLATFORM_REGISTRY_MOUNT_POINT
#define REG_PATH    MOUNT_POINT "/registry.bin"
#define MOUNT_POINT_MAX (sizeof(MOUNT_POINT) + APP_NAME_MAX + 8)

/* Build the on-flash path for one of an application's files: <mount>/<name><ext> */
static int app_path(char *out, size_t cap, const char *name, const char *ext)
{
	int n = snprintk(out, cap, "%s/%s%s", MOUNT_POINT, name, ext);

	return (n > 0 && (size_t)n < cap) ? 0 : -ENAMETOOLONG;
}

static int blob_path(char *out, size_t cap, const char *name)
{
	return app_path(out, cap, name, ".llext");
}

int app_registry_blob_path(const char *name, char *out, size_t cap)
{
	return blob_path(out, cap, name);
}

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_cfg,
	.storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
	.mnt_point = MOUNT_POINT,
};

/* On-flash registry: a header plus the records. */
struct reg_file {
	uint32_t magic;
	uint32_t count;
	uint32_t next_seq;
	struct app_record apps[APP_REG_MAX];
};
/* Bumped when the record layout changes; an older on-flash registry whose
 * magic differs is treated as absent and rebuilt rather than misread.
 */
#define REG_MAGIC 0x41505073U

static struct reg_file reg;

static int reg_save(void)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	int rc = fs_open(&f, REG_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc != 0) {
		return rc;
	}
	rc = fs_write(&f, &reg, sizeof(reg));
	fs_close(&f);
	return (rc == sizeof(reg)) ? 0 : -EIO;
}

int app_registry_load(void)
{
	int rc = fs_mount(&mnt);

	if (rc != 0 && rc != -EBUSY) {
		LOG_ERR("mount failed %d", rc);
		return rc;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, REG_PATH, FS_O_READ);
	if (rc == 0) {
		ssize_t n = fs_read(&f, &reg, sizeof(reg));

		fs_close(&f);
		if (n == sizeof(reg) && reg.magic == REG_MAGIC) {
			LOG_INF("loaded %u installed app(s) from flash", reg.count);
			return 0;
		}
	}

	/* First boot: empty registry. */
	memset(&reg, 0, sizeof(reg));
	reg.magic = REG_MAGIC;
	reg.next_seq = 1;
	LOG_INF("no registry found, starting empty (first boot)");
	return reg_save();
}

static int find(const char *name)
{
	for (uint32_t i = 0; i < reg.count; i++) {
		if (strcmp(reg.apps[i].name, name) == 0) {
			return (int)i;
		}
	}
	return -1;
}

int app_registry_install_versioned(const char *name, bool autostart,
				   uint32_t version)
{
	int idx = find(name);

	if (idx >= 0 && version < reg.apps[idx].version) {
		LOG_WRN("rejected '%s' v%u: older than installed v%u (rollback)",
			name, version, reg.apps[idx].version);
		return -EACCES;
	}

	if (idx < 0) {
		if (reg.count >= APP_REG_MAX) {
			return -ENOSPC;
		}
		idx = reg.count++;
		strncpy(reg.apps[idx].name, name, APP_NAME_MAX - 1);
		reg.apps[idx].name[APP_NAME_MAX - 1] = '\0';
	}
	reg.apps[idx].enabled = 1;
	reg.apps[idx].autostart = autostart ? 1 : 0;
	reg.apps[idx].install_seq = reg.next_seq++;
	reg.apps[idx].version = version;
	LOG_INF("installed '%s' (seq %u, v%u, autostart=%d)", name,
		reg.apps[idx].install_seq, version, reg.apps[idx].autostart);
	return reg_save();
}

int app_registry_install(const char *name, bool autostart)
{
	return app_registry_install_versioned(name, autostart, 0);
}

int app_registry_count(void)
{
	return (int)reg.count;
}

int app_registry_get(int idx, struct app_record *out)
{
	if (idx < 0 || idx >= (int)reg.count) {
		return -EINVAL;
	}
	*out = reg.apps[idx];
	return 0;
}

int app_registry_uninstall(const char *name)
{
	int idx = find(name);

	if (idx < 0) {
		return -ENOENT;
	}
	for (uint32_t i = idx; i + 1 < reg.count; i++) {
		reg.apps[i] = reg.apps[i + 1];
	}
	reg.count--;

	/* Drop the persisted binary and manifest too, if present. */
	char path[MOUNT_POINT_MAX];

	if (blob_path(path, sizeof(path), name) == 0) {
		(void)fs_unlink(path);
	}
	if (app_path(path, sizeof(path), name, ".manifest") == 0) {
		(void)fs_unlink(path);
	}

	LOG_INF("uninstalled '%s'", name);
	return reg_save();
}

int app_registry_store_blob(const char *name, const uint8_t *buf, size_t len)
{
	char path[MOUNT_POINT_MAX];
	int rc = blob_path(path, sizeof(path), name);

	if (rc != 0) {
		return rc;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}

	ssize_t n = fs_write(&f, buf, len);

	fs_close(&f);
	if (n < 0 || (size_t)n != len) {
		return -EIO;
	}
	LOG_INF("stored binary for '%s' (%zu bytes)", name, len);
	return 0;
}

int app_registry_load_blob(const char *name, uint8_t *buf, size_t cap,
			   size_t *out_len)
{
	char path[MOUNT_POINT_MAX];
	int rc = blob_path(path, sizeof(path), name);

	if (rc != 0) {
		return rc;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}

	ssize_t n = fs_read(&f, buf, cap);

	fs_close(&f);
	if (n < 0) {
		return -EIO;
	}
	*out_len = (size_t)n;
	return 0;
}

/* The signed manifest and its signature are stored as one file:
 * the manifest body followed by the 64-byte signature.
 */
int app_registry_store_manifest(const char *name, const struct app_manifest *mf,
				const uint8_t *sig, size_t sig_len)
{
	char path[MOUNT_POINT_MAX];
	int rc = app_path(path, sizeof(path), name, ".manifest");

	if (rc != 0) {
		return rc;
	}
	if (sig_len != APP_MANIFEST_SIG_LEN) {
		return -EINVAL;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}

	ssize_t n = fs_write(&f, mf, sizeof(*mf));

	if (n == (ssize_t)sizeof(*mf)) {
		n = fs_write(&f, sig, sig_len);
		n = (n == (ssize_t)sig_len) ? 0 : -EIO;
	} else {
		n = -EIO;
	}
	fs_close(&f);
	if (n != 0) {
		return -EIO;
	}
	LOG_INF("stored manifest for '%s' (v%u)", name, mf->version);
	return 0;
}

int app_registry_load_manifest(const char *name, struct app_manifest *mf,
			       uint8_t *sig, size_t sig_cap)
{
	char path[MOUNT_POINT_MAX];
	int rc = app_path(path, sizeof(path), name, ".manifest");

	if (rc != 0) {
		return rc;
	}
	if (sig_cap < APP_MANIFEST_SIG_LEN) {
		return -EINVAL;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}

	ssize_t n = fs_read(&f, mf, sizeof(*mf));

	if (n == (ssize_t)sizeof(*mf)) {
		n = fs_read(&f, sig, APP_MANIFEST_SIG_LEN);
		n = (n == APP_MANIFEST_SIG_LEN) ? 0 : -EIO;
	} else {
		n = -EIO;
	}
	fs_close(&f);
	return (n == 0) ? 0 : -EIO;
}
