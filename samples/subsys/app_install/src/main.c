/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Over-the-wire app install over a single serial cable.
 *
 * A host signs an application on a build machine, then uploads two files over
 * the shell's mcumgr transport into the device's storage:
 *   mcumgr ... fs upload app.llext     /lfs/<name>.llext
 *   mcumgr ... fs upload app.manifest  /lfs/<name>.manifest
 * The shell command "app install <name>" then verifies the signed manifest
 * against the public key compiled into the firmware, binds the module to it,
 * records it in the registry, and launches it as an isolated user-mode app -
 * streamed from flash, never held whole in RAM. "app list" shows what is
 * installed. The same cable carries both the upload and the shell, so no extra
 * wiring or second port is needed.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <string.h>

/* Trusted public key: only an app signed by the matching host key is accepted.
 * The bundled reference app's bytes are unused at runtime; the key is what
 * matters for verifying uploads.
 */
static const uint8_t trusted_pubkey[] = {
#include "install_app.pubkey.inc"
};

#define APP_CAPS    (APP_CAP_LOG | APP_CAP_TIME)
#define MOUNT_POINT CONFIG_APP_PLATFORM_REGISTRY_MOUNT_POINT

static int manifest_path(char *out, size_t cap, const char *name)
{
	int n = snprintk(out, cap, "%s/%s.manifest", MOUNT_POINT, name);

	return (n > 0 && (size_t)n < cap) ? 0 : -ENAMETOOLONG;
}

/* Read an uploaded manifest file into the manifest struct + signature. */
static int read_uploaded_manifest(const char *name, struct app_manifest *mf,
				  uint8_t *sig, size_t sig_cap)
{
	char path[64];

	if (manifest_path(path, sizeof(path), name) != 0 ||
	    sig_cap < APP_MANIFEST_SIG_LEN) {
		return -EINVAL;
	}

	struct fs_file_t f;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		return -ENOENT;
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

static int cmd_install(const struct shell *sh, size_t argc, char **argv)
{
	const char *name = argv[1];
	struct app_manifest mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];

	if (read_uploaded_manifest(name, &mf, sig, sizeof(sig)) != 0) {
		shell_error(sh, "no uploaded manifest for '%s' "
			    "(upload /lfs/%s.manifest first)", name, name);
		return -ENOENT;
	}
	if (strncmp(mf.name, name, APP_MANIFEST_NAME_MAX) != 0) {
		shell_error(sh, "manifest names '%s', not '%s' - refused",
			    mf.name, name);
		return -EPERM;
	}

	/* The module file uploaded as /lfs/<name>.llext. */
	char path[64];

	if (app_registry_blob_path(name, path, sizeof(path)) != 0) {
		shell_error(sh, "bad path");
		return -EINVAL;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load_manifest_file(&mf, sig,
						      APP_MANIFEST_SIG_LEN, path,
						      trusted_pubkey,
						      ARRAY_SIZE(trusted_pubkey),
						      "app_main", &ext, &entry);

	if (rc != 0) {
		shell_error(sh, "verify/load failed (%d) - install rejected", rc);
		return rc;
	}

	rc = app_registry_install_versioned(name, true, mf.version);
	if (rc != 0) {
		shell_error(sh, "registry install failed (%d)", rc);
		return rc;
	}

	rc = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry,
				       APP_CAPS);
	if (rc < 0) {
		shell_error(sh, "launch refused (%d)", rc);
		return rc;
	}
	shell_print(sh, "installed and launched '%s' v%u (isolated app %d)",
		    mf.name, mf.version, rc);
	return 0;
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	int n = app_registry_count();

	if (n == 0) {
		shell_print(sh, "no apps installed");
		return 0;
	}
	for (int i = 0; i < n; i++) {
		struct app_record r;

		if (app_registry_get(i, &r) == 0) {
			shell_print(sh, "  %s  v%u%s", r.name, r.version,
				    r.autostart ? "  (autostart)" : "");
		}
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(app_cmds,
	SHELL_CMD_ARG(install, NULL, "install <name>: verify+launch an uploaded app",
		      cmd_install, 2, 0),
	SHELL_CMD(list, NULL, "list installed apps", cmd_list),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(app, &app_cmds, "App platform commands", NULL);

int main(void)
{
	printk("\n*** over-the-wire app install (C5) ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}
	if (app_registry_load() != 0) {
		printk("registry load failed\n");
		return 0;
	}
	printk("ready. upload a signed app, then: app install <name>\n");
	return 0;
}
