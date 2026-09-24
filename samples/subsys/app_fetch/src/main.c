/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App store over Wi-Fi: the device pulls a signed application from a server.
 *
 * At boot the device joins Wi-Fi and waits for an address. The shell command
 *   store fetch <name> <server-ip>
 * downloads two files over HTTP into the registry's staged paths:
 *   http://<server-ip>:8000/<name>.llext     -> /lfs/<name>.llext.new
 *   http://<server-ip>:8000/<name>.manifest  -> /lfs/<name>.manifest.new
 * then verifies the signed manifest against the public key compiled into the
 * firmware, binds the module to it, moves both over the installed copies
 * unless that would be a downgrade, records it in the registry, and launches
 * it as an isolated user-mode app. A tampered, unsigned or older download is
 * rejected and leaves the installed application untouched. This is the app
 * store pulling a release from a server.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_loader.h>
#include <zephyr/app_platform/app_registry.h>
#include <zephyr/app_platform/app_manifest.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/http/client.h>
#include <string.h>

static const uint8_t trusted_pubkey[] = {
#include "fetch_app.pubkey.inc"
};

#define APP_CAPS     (APP_CAP_LOG | APP_CAP_TIME)
#define MOUNT_POINT  CONFIG_APP_PLATFORM_REGISTRY_MOUNT_POINT
#define SERVER_PORT  8000

static K_SEM_DEFINE(got_ip, 0, 1);
static struct net_mgmt_event_callback ipv4_cb;

static void ipv4_evt(struct net_mgmt_event_callback *cb, uint64_t ev,
		     struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (ev == NET_EVENT_IPV4_ADDR_ADD) {
		k_sem_give(&got_ip);
	}
}

static void wifi_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	static struct wifi_connect_req_params p;

	p.ssid = (const uint8_t *)CONFIG_APP_FETCH_WIFI_SSID;
	p.ssid_length = strlen(CONFIG_APP_FETCH_WIFI_SSID);
	p.psk = (const uint8_t *)CONFIG_APP_FETCH_WIFI_PSK;
	p.psk_length = strlen(CONFIG_APP_FETCH_WIFI_PSK);
	p.security = WIFI_SECURITY_TYPE_PSK;
	p.channel = WIFI_CHANNEL_ANY;
	p.band = WIFI_FREQ_BAND_UNKNOWN;
	p.mfp = WIFI_MFP_OPTIONAL;

	printk("connecting to Wi-Fi '%s'...\n", CONFIG_APP_FETCH_WIFI_SSID);
	if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p))) {
		printk("Wi-Fi connect request failed\n");
		return;
	}

	/* Turn off power save so the station stays awake for the transfer;
	 * with it on, some access points drop the link between beacons.
	 */
	struct wifi_ps_params ps = { .enabled = WIFI_PS_DISABLED };

	(void)net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));
}

/* A small file written incrementally as HTTP body chunks arrive. */
struct dl {
	struct fs_file_t f;
	bool open;
	int err;
};

static int dl_http_cb(struct http_response *rsp, enum http_final_call final,
		      void *user)
{
	struct dl *d = user;

	ARG_UNUSED(final);
	if (d->err) {
		return d->err;
	}
	if (rsp->body_frag_len && rsp->body_frag_start) {
		ssize_t n = fs_write(&d->f, rsp->body_frag_start,
				     rsp->body_frag_len);

		if (n < 0 || (size_t)n != rsp->body_frag_len) {
			d->err = -EIO;
		}
	}
	return 0;
}

/*
 * GET http://<host>:8000/<remote> into <local>.
 *
 * Progress goes to @p sh, not to printk: a shell command's own output is
 * ordered against the rest of the command, while printk from the same thread
 * is deferred and surfaces after the command has already reported its result.
 * A NULL @p sh silences the progress lines (the boot path has no shell).
 */
static int http_get_to_file(const struct shell *sh, const char *host,
			    const char *remote, const char *local)
{
	struct sockaddr_in addr = {0};

	addr.sin_family = AF_INET;
	addr.sin_port = htons(SERVER_PORT);
	if (net_addr_pton(AF_INET, host, &addr.sin_addr) != 0) {
		return -EINVAL;
	}

	int sock = -1;
	int crc = -1;

	for (int attempt = 0; attempt < 5; attempt++) {
		sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock < 0) {
			if (sh != NULL) {
				shell_error(sh, "  socket() failed: %d", errno);
			}
			return -errno;
		}
		crc = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
		if (crc == 0) {
			break;
		}
		if (sh != NULL) {
			shell_warn(sh, "  connect attempt %d to %s:%d failed: %d",
				   attempt + 1, host, SERVER_PORT, errno);
		}
		zsock_close(sock);
		sock = -1;
		k_msleep(500);
	}
	if (crc != 0) {
		return -ETIMEDOUT;
	}
	if (sh != NULL) {
		shell_print(sh, "  connected to %s:%d", host, SERVER_PORT);
	}

	struct dl d = {0};

	fs_file_t_init(&d.f);
	if (fs_open(&d.f, local, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) != 0) {
		zsock_close(sock);
		return -EIO;
	}

	static uint8_t recv_buf[512];
	struct http_request req = {0};

	req.method = HTTP_GET;
	req.url = remote;
	req.host = host;
	req.protocol = "HTTP/1.1";
	req.response = dl_http_cb;
	req.recv_buf = recv_buf;
	req.recv_buf_len = sizeof(recv_buf);

	int rc = http_client_req(sock, &req, 10000, &d);

	if (sh != NULL) {
		shell_print(sh, "  http_client_req rc=%d, body_err=%d", rc, d.err);
	}
	fs_close(&d.f);
	zsock_close(sock);

	if (rc < 0) {
		return rc;
	}
	return d.err;
}

/*
 * Load and launch one installed application from flash. The manifest and the
 * module are re-verified against the trusted key on every boot, so a record in
 * the registry is not by itself permission to run: the signature still has to
 * check against the image on flash.
 */
static int autostart_one(const struct app_record *rec, void *user)
{
	ARG_UNUSED(user);
	struct app_manifest mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];
	char lpath[64];

	if (app_registry_load_manifest(rec->name, &mf, sig, sizeof(sig)) != 0) {
		return -ENOENT;
	}
	if (app_registry_blob_path(rec->name, lpath, sizeof(lpath)) != 0) {
		return -ENAMETOOLONG;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load_manifest_file(&mf, sig,
						      APP_MANIFEST_SIG_LEN, lpath,
						      trusted_pubkey,
						      ARRAY_SIZE(trusted_pubkey),
						      "app_main", &ext, &entry);

	if (rc != 0) {
		return rc;
	}
	rc = app_registry_check(rec, &mf);
	if (rc == 0) {
		rc = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, APP_CAPS);
	}
	if (rc < 0) {
		app_loader_forget(ext);
		llext_unload(&ext);
		return rc;
	}
	printk("autostarted '%s' v%u (isolated app %d)\n", mf.name, mf.version, rc);
	return rc;
}

static int cmd_fetch(const struct shell *sh, size_t argc, char **argv)
{
	const char *name = argv[1];
	const char *host = argv[2];
	char rpath[48];
	char lpath[64];

	shell_print(sh, "fetching '%s' from %s ...", name, host);

	/* Manifest first, then the module, both to the staged paths. */
	snprintk(rpath, sizeof(rpath), "/%s.manifest", name);
	if (app_registry_staged_manifest_path(name, lpath, sizeof(lpath)) != 0 ||
	    http_get_to_file(sh, host, rpath, lpath) != 0) {
		shell_error(sh, "manifest download failed");
		app_registry_discard(name);
		return -EIO;
	}

	snprintk(rpath, sizeof(rpath), "/%s.llext", name);
	if (app_registry_staged_blob_path(name, lpath, sizeof(lpath)) != 0 ||
	    http_get_to_file(sh, host, rpath, lpath) != 0) {
		shell_error(sh, "module download failed");
		app_registry_discard(name);
		return -EIO;
	}
	shell_print(sh, "downloaded; verifying signature ...");

	struct app_manifest mf;
	uint8_t sig[APP_MANIFEST_SIG_LEN];

	if (app_registry_load_staged_manifest(name, &mf, sig, sizeof(sig)) != 0) {
		shell_error(sh, "manifest unreadable");
		app_registry_discard(name);
		return -EIO;
	}
	if (strncmp(mf.name, name, APP_MANIFEST_NAME_MAX) != 0) {
		shell_error(sh, "manifest names '%s', not '%s' - refused",
			    mf.name, name);
		app_registry_discard(name);
		return -EPERM;
	}

	struct llext *ext = NULL;
	void *entry = NULL;
	int rc = app_loader_verify_load_manifest_file(&mf, sig,
						      APP_MANIFEST_SIG_LEN, lpath,
						      trusted_pubkey,
						      ARRAY_SIZE(trusted_pubkey),
						      "app_main", &ext, &entry);

	if (rc != 0) {
		shell_error(sh, "verify/load failed (%d) - rejected", rc);
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

	rc = app_platform_launch_llext(mf.name, ext, (app_entry_t)entry, APP_CAPS);
	if (rc < 0) {
		shell_error(sh, "launch refused (%d)", rc);
		app_loader_forget(ext);
		llext_unload(&ext);
		return rc;
	}
	shell_print(sh, "fetched, verified and launched '%s' v%u (isolated app %d)",
		    mf.name, mf.version, rc);
	return 0;
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (int i = 0; i < app_registry_count(); i++) {
		struct app_record r;
		struct app_manifest mf;
		uint8_t sig[APP_MANIFEST_SIG_LEN];

		if (app_registry_get(i, &r) != 0) {
			continue;
		}
		if (app_registry_load_manifest(r.name, &mf, sig, sizeof(sig)) != 0) {
			mf.version = 0U;
		}
		shell_print(sh, "%s: record v%u, manifest on flash v%u, bound=%u, autostart=%u",
			    r.name, r.version, mf.version, r.bound, r.autostart);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(store_cmds,
	SHELL_CMD_ARG(fetch, NULL, "fetch <name> <server-ip>: download+verify+launch",
		      cmd_fetch, 3, 0),
	SHELL_CMD(list, NULL, "list installed applications", cmd_list),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(store, &store_cmds, "App store: fetch apps over the network",
		   NULL);

int main(void)
{
	printk("\n*** app store over Wi-Fi (C5) ***\n");

	if (app_platform_init() != 0 || app_registry_load() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	/* Relaunch what is installed before touching the network: an installed
	 * application is expected to be running after a reboot, and its image
	 * is already on flash.
	 */
	int n = app_registry_autostart_all(autostart_one, NULL);

	printk("autostart: %d of %d installed apps launched\n", n,
	       app_registry_count());

	/* Let the Wi-Fi interface finish coming up, then issue a single
	 * connection request and wait for an address. The connect is
	 * asynchronous, so re-requesting while it is in progress only confuses
	 * the driver; wait it out instead.
	 */
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_evt, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	struct net_if *iface = net_if_get_first_wifi();

	for (int i = 0; i < 50 && (iface == NULL || !net_if_is_admin_up(iface)); i++) {
		k_msleep(100);
		iface = net_if_get_first_wifi();
	}
	k_msleep(1000);

	wifi_connect();
	if (k_sem_take(&got_ip, K_SECONDS(45)) == 0) {
		printk("online. fetch an app with: store fetch <name> "
		       "<server-ip>\n");
		return 0;
	}
	printk("no IP address; check Wi-Fi credentials/band (2.4GHz)\n");
	return 0;
}
