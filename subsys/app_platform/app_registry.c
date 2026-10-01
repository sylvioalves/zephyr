/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The installed-application registry, persisted to a littlefs partition so the
 * set of installed applications survives reboots.
 */

#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_manifest.h>
#ifdef CONFIG_APP_PLATFORM_LOADER
#include <zephyr/app_platform/app_abi.h>
#include <zephyr/app_platform_abi.h>
#endif

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
#define REG_TMP_PATH    MOUNT_POINT "/registry.new"
#define MOUNT_POINT_MAX (sizeof(MOUNT_POINT) + APP_NAME_MAX + 16)
#define STAGED_SUFFIX   ".new"

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

int app_registry_staged_blob_path(const char *name, char *out, size_t cap)
{
	return app_path(out, cap, name, ".llext" STAGED_SUFFIX);
}

int app_registry_staged_manifest_path(const char *name, char *out, size_t cap)
{
	return app_path(out, cap, name, ".manifest" STAGED_SUFFIX);
}

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_cfg,
	.storage_dev = (void *)PARTITION_ID(storage_partition),
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
#define REG_MAGIC 0x41505074U

static struct reg_file reg;

/* Result of the last autostart launch of each record, in RAM only. */
static int autostart_err[APP_REG_MAX];

/* Write the registry to a temporary file and rename it over the live one, so
 * an interrupted save leaves the previous registry intact rather than a
 * truncated file.
 */
static int reg_save(void)
{
	struct fs_file_t f;

	fs_file_t_init(&f);
	int rc = fs_open(&f, REG_TMP_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc != 0) {
		return rc;
	}
	rc = fs_write(&f, &reg, sizeof(reg));
	if (rc == sizeof(reg)) {
		rc = fs_sync(&f);
	} else if (rc >= 0) {
		rc = -EIO;
	}
	fs_close(&f);
	if (rc != 0) {
		(void)fs_unlink(REG_TMP_PATH);
		return rc;
	}
	rc = fs_rename(REG_TMP_PATH, REG_PATH);
	if (rc != 0) {
		(void)fs_unlink(REG_TMP_PATH);
	}
	return rc;
}

int app_registry_load(void)
{
	int rc = fs_mount(&mnt);

	memset(autostart_err, 0, sizeof(autostart_err));
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
		if (n == sizeof(reg) && reg.magic == REG_MAGIC && reg.count <= APP_REG_MAX) {
			LOG_INF("loaded %u installed app(s) from flash", reg.count);
			return 0;
		}
		if (n == sizeof(reg) && reg.magic == REG_MAGIC) {
			LOG_WRN("registry holds %u records, max %u: rebuilding", reg.count,
				(unsigned int)APP_REG_MAX);
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
	/* The manifest carries a longer name than a record holds. Truncating
	 * here would let two signed applications sharing a prefix collide on
	 * one record and one blob, so refuse the name instead.
	 */
	if (strnlen(name, APP_NAME_MAX) >= APP_NAME_MAX) {
		LOG_WRN("rejected '%s': name longer than %u characters", name,
			(unsigned int)(APP_NAME_MAX - 1));
		return -ENAMETOOLONG;
	}

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
	reg.apps[idx].bound = 0U;
	autostart_err[idx] = 0;
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

static const char *refusal_reason(int err)
{
	switch (err) {
	case -EPERM:
		return "signature invalid or required capabilities not allowed";
	case -ENOEXEC:
		return "built for another interface major version or target";
	case -ENOTSUP:
		return "manifest format retired, install the application again";
	case -EACCES:
		return "older than the installed version or imports a forbidden symbol";
	case -EBADMSG:
		return "manifest malformed or not the installed image";
	case -ENXIO:
		return "signed with a key this firmware does not trust";
	case -ECANCELED:
		return "signed with a revoked key, or the configure hook refused it";
	default:
		return "launch failed";
	}
}

int app_registry_autostart_all(app_registry_launch_t launch, void *user)
{
	int started = 0;

	if (launch == NULL) {
		return -EINVAL;
	}
	for (uint32_t i = 0; i < reg.count; i++) {
		const struct app_record *r = &reg.apps[i];

		if (r->enabled == 0U || r->autostart == 0U) {
			continue;
		}
		int rc = launch(r, user);

		autostart_err[i] = (rc < 0) ? rc : 0;
		if (rc < 0) {
			LOG_ERR("autostart of '%s' refused: %s (%d)", r->name, refusal_reason(rc),
				rc);
			continue;
		}
		started++;
	}
	return started;
}

int app_registry_autostart_result(const char *name, int *err)
{
	int idx = find(name);

	if (idx < 0 || err == NULL) {
		return -ENOENT;
	}
	*err = autostart_err[idx];
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
		autostart_err[i] = autostart_err[i + 1];
	}
	reg.count--;
	autostart_err[reg.count] = 0;

	/* Drop the persisted binary and manifest too, if present. */
	char path[MOUNT_POINT_MAX];

	if (blob_path(path, sizeof(path), name) == 0) {
		(void)fs_unlink(path);
	}
	if (app_path(path, sizeof(path), name, ".manifest") == 0) {
		(void)fs_unlink(path);
	}
	app_registry_discard(name);

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
static int store_manifest_at(const char *name, const char *ext, const struct app_manifest *mf,
			     size_t mf_len, const uint8_t *sig, size_t sig_len)
{
	char path[MOUNT_POINT_MAX];
	int rc = app_path(path, sizeof(path), name, ext);

	if (rc != 0) {
		return rc;
	}
	if (sig_len != APP_MANIFEST_SIG_LEN || mf_len == 0U || mf_len > APP_MANIFEST_MAX_LEN) {
		return -EINVAL;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}

	ssize_t n = fs_write(&f, mf, mf_len);

	if (n == (ssize_t)mf_len) {
		n = fs_write(&f, sig, sig_len);
		n = (n == (ssize_t)sig_len) ? 0 : -EIO;
	} else {
		n = -EIO;
	}
	fs_close(&f);
	if (n != 0) {
		return -EIO;
	}
	LOG_INF("stored manifest for '%s'", name);
	return 0;
}

int app_registry_store_manifest(const char *name, const struct app_manifest *mf, size_t mf_len,
				const uint8_t *sig, size_t sig_len)
{
	return store_manifest_at(name, ".manifest", mf, mf_len, sig, sig_len);
}

int app_registry_store_staged_manifest(const char *name, const struct app_manifest *mf,
				       size_t mf_len, const uint8_t *sig, size_t sig_len)
{
	return store_manifest_at(name, ".manifest" STAGED_SUFFIX, mf, mf_len, sig, sig_len);
}

static int load_manifest_at(const char *name, const char *ext, union app_manifest_buf *mf,
			    size_t *mf_len, uint8_t *sig, size_t sig_cap)
{
	char path[MOUNT_POINT_MAX];
	int rc = app_path(path, sizeof(path), name, ext);

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

	off_t size = -EIO;
	size_t len = 0U;

	if (fs_seek(&f, 0, FS_SEEK_END) == 0) {
		size = fs_tell(&f);
	}
	if (size <= (off_t)APP_MANIFEST_SIG_LEN) {
		rc = -EIO;
	} else if (size > (off_t)(APP_MANIFEST_MAX_LEN + APP_MANIFEST_SIG_LEN)) {
		rc = -EFBIG;
	} else {
		len = (size_t)size - APP_MANIFEST_SIG_LEN;
		memset(mf, 0, sizeof(*mf));
		rc = fs_seek(&f, 0, FS_SEEK_SET);
	}
	if (rc == 0) {
		if (fs_read(&f, mf->raw, len) != (ssize_t)len ||
		    fs_read(&f, sig, APP_MANIFEST_SIG_LEN) != APP_MANIFEST_SIG_LEN) {
			rc = -EIO;
		}
	}
	fs_close(&f);
	if (rc == 0) {
		*mf_len = len;
	}
	return rc;
}

int app_registry_load_manifest(const char *name, union app_manifest_buf *mf, size_t *mf_len,
			       uint8_t *sig, size_t sig_cap)
{
	return load_manifest_at(name, ".manifest", mf, mf_len, sig, sig_cap);
}

int app_registry_load_staged_manifest(const char *name, union app_manifest_buf *mf,
				      size_t *mf_len, uint8_t *sig, size_t sig_cap)
{
	return load_manifest_at(name, ".manifest" STAGED_SUFFIX, mf, mf_len, sig, sig_cap);
}

void app_registry_discard(const char *name)
{
	char path[MOUNT_POINT_MAX];

	if (app_registry_staged_blob_path(name, path, sizeof(path)) == 0) {
		(void)fs_unlink(path);
	}
	if (app_registry_staged_manifest_path(name, path, sizeof(path)) == 0) {
		(void)fs_unlink(path);
	}
}

static int promote(const char *name, const char *staged_ext, const char *ext)
{
	char from[MOUNT_POINT_MAX];
	char to[MOUNT_POINT_MAX];
	int rc = app_path(from, sizeof(from), name, staged_ext);

	if (rc == 0) {
		rc = app_path(to, sizeof(to), name, ext);
	}
	if (rc == 0) {
		rc = fs_rename(from, to);
	}
	return rc;
}

/* Whether a manifest names the interface major version and target of this
 * firmware.
 */
static bool loads_here(const struct app_manifest *mf)
{
#ifdef CONFIG_APP_PLATFORM_LOADER
	return mf->abi_major == APP_ABI_MAJOR && mf->target == APP_PLATFORM_TARGET;
#else
	ARG_UNUSED(mf);
	return false;
#endif
}

/* Whether the installed manifest of @p name describes an image this firmware
 * refuses to load: one of a retired format, another interface major version or
 * another target. An image signed with a revoked or unknown key does not count,
 * so revoking a key never lets an older image replace a newer one.
 */
static bool installed_cannot_load(const char *name)
{
	union app_manifest_buf mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	size_t len = 0U;

	if (app_registry_load_manifest(name, &mf, &len, sig, sizeof(sig)) != 0 ||
	    len < sizeof(mf.mf.magic)) {
		return false;
	}
	if (app_manifest_retired_format(mf.mf.magic) != 0U) {
		return true;
	}
	return mf.mf.magic == APP_MANIFEST_MAGIC && len >= sizeof(mf.mf) && !loads_here(&mf.mf);
}

int app_registry_commit(const char *name, bool autostart, const struct app_manifest *mf)
{
	if (strncmp(mf->name, name, APP_MANIFEST_NAME_MAX) != 0) {
		return -EINVAL;
	}
	if (strnlen(name, APP_NAME_MAX) >= APP_NAME_MAX) {
		app_registry_discard(name);
		return -ENAMETOOLONG;
	}

	int idx = find(name);

	if (idx >= 0 && mf->version < reg.apps[idx].version) {
		if (!installed_cannot_load(name) || !loads_here(mf)) {
			LOG_WRN("rejected '%s' v%u: older than installed v%u (rollback)",
				name, mf->version, reg.apps[idx].version);
			app_registry_discard(name);
			return -EACCES;
		}
		LOG_WRN("installing '%s' v%u over v%u, which cannot load on this firmware", name,
			mf->version, reg.apps[idx].version);
	}
	if (idx < 0 && reg.count >= APP_REG_MAX) {
		app_registry_discard(name);
		return -ENOSPC;
	}

	/* The module moves first: a manifest left behind no longer matches it. */
	int rc = promote(name, ".llext" STAGED_SUFFIX, ".llext");

	if (rc == 0) {
		rc = promote(name, ".manifest" STAGED_SUFFIX, ".manifest");
	}
	if (rc != 0) {
		LOG_ERR("could not install staged '%s' (%d)", name, rc);
		return rc;
	}

	if (idx < 0) {
		idx = reg.count++;
		strncpy(reg.apps[idx].name, name, APP_NAME_MAX - 1);
		reg.apps[idx].name[APP_NAME_MAX - 1] = '\0';
	}
	reg.apps[idx].enabled = 1;
	reg.apps[idx].autostart = autostart ? 1 : 0;
	reg.apps[idx].install_seq = reg.next_seq++;
	reg.apps[idx].version = mf->version;
	reg.apps[idx].bound = 1U;
	autostart_err[idx] = 0;
	memcpy(reg.apps[idx].module_sha256, mf->module_sha256,
	       sizeof(reg.apps[idx].module_sha256));
	LOG_INF("committed '%s' v%u (seq %u)", name, mf->version, reg.apps[idx].install_seq);
	return reg_save();
}

int app_registry_check(const struct app_record *rec, const struct app_manifest *mf)
{
	if (strncmp(mf->name, rec->name, APP_NAME_MAX) != 0) {
		return -EINVAL;
	}
	if (mf->version < rec->version) {
		LOG_WRN("refused '%s' v%u: installed record is v%u", rec->name, mf->version,
			rec->version);
		return -EACCES;
	}
	if (rec->bound != 0U &&
	    memcmp(mf->module_sha256, rec->module_sha256, sizeof(rec->module_sha256)) != 0) {
		LOG_WRN("refused '%s': module is not the committed image", rec->name);
		return -EBADMSG;
	}
	return 0;
}
