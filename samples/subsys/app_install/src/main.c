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
 *   mcumgr ... fs upload app.llext     /lfs/<name>.llext.new
 *   mcumgr ... fs upload app.manifest  /lfs/<name>.manifest.new
 * The shell command "store install <name>" then verifies the signed manifest
 * against the public key compiled into the firmware, binds the module to it,
 * moves both over the installed copies unless that would be a downgrade,
 * records it in the registry, and launches it as an isolated user-mode app -
 * streamed from flash, never held whole in RAM. "store list" shows what is
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

static int cmd_install(const struct shell *sh, size_t argc, char **argv)
{
	const char *name = argv[1];
	struct app_manifest mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];

	if (app_registry_load_staged_manifest(name, &mf, sig, sizeof(sig)) != 0) {
		shell_error(sh, "no uploaded manifest for '%s' "
			    "(upload /lfs/%s.manifest.new first)", name, name);
		return -ENOENT;
	}
	if (strncmp(mf.name, name, APP_MANIFEST_NAME_MAX) != 0) {
		shell_error(sh, "manifest names '%s', not '%s' - refused",
			    mf.name, name);
		app_registry_discard(name);
		return -EPERM;
	}

	char path[64];

	if (app_registry_staged_blob_path(name, path, sizeof(path)) != 0) {
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
		app_registry_discard(name);
		return rc;
	}

	rc = app_registry_commit(name, true, &mf);
	if (rc != 0) {
		shell_error(sh, "install refused (%d)", rc);
		app_loader_forget(ext);
		llext_unload(&ext);
		return rc;
	}

	rc = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, NULL,
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

SHELL_STATIC_SUBCMD_SET_CREATE(store_cmds,
	SHELL_CMD_ARG(install, NULL, "install <name>: verify+launch an uploaded app",
		      cmd_install, 2, 0),
	SHELL_CMD(list, NULL, "list installed apps", cmd_list),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(store, &store_cmds, "App store: install uploaded apps", NULL);

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
	printk("ready. upload a signed app, then: store install <name>\n");
	return 0;
}
