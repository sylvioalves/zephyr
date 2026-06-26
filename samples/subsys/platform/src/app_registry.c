/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_registry.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>
#include <errno.h>

#define MOUNT_POINT "/lfs"
#define REG_PATH    MOUNT_POINT "/registry.bin"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);
static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_cfg,
	.storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
	.mnt_point = MOUNT_POINT,
};

/* On-flash registry: a header count plus the records. */
struct reg_file {
	uint32_t magic;
	uint32_t count;
	uint32_t next_seq;
	struct app_record apps[APP_REG_MAX];
};
#define REG_MAGIC 0x41505052   /* "APPR" */

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
		printk("registry: mount failed %d\n", rc);
		return rc;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	rc = fs_open(&f, REG_PATH, FS_O_READ);
	if (rc == 0) {
		ssize_t n = fs_read(&f, &reg, sizeof(reg));

		fs_close(&f);
		if (n == sizeof(reg) && reg.magic == REG_MAGIC) {
			printk("registry: loaded %u installed app(s) from flash\n",
			       reg.count);
			return 0;
		}
	}

	/* First boot: empty registry. */
	memset(&reg, 0, sizeof(reg));
	reg.magic = REG_MAGIC;
	reg.next_seq = 1;
	printk("registry: no registry found, starting empty (first boot)\n");
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

int app_registry_install(const char *name, bool autostart)
{
	int idx = find(name);

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
	printk("registry: installed '%s' (seq %u, autostart=%d)\n",
	       name, reg.apps[idx].install_seq, reg.apps[idx].autostart);
	return reg_save();
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
	printk("registry: uninstalled '%s'\n", name);
	return reg_save();
}
