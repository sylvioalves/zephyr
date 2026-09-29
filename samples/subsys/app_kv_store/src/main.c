/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Two isolated user-mode applications using the platform's key/value store.
 * One walks a key through its whole life - set, read back, overwrite, delete,
 * and confirm it is gone - while the other checks that the store is namespaced
 * per application: the same key name in each holds a different value, and
 * neither can delete the other's.
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/app_platform/app_services.h>
#include <zephyr/app_platform/app_caps.h>
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

K_APPMEM_PARTITION_DEFINE(kv_part);
K_APP_DMEM(kv_part) static int passes;
K_APP_DMEM(kv_part) static int fails;

static void check(bool ok, const char *what, int rc)
{
	char line[72];

	if (ok) {
		(void)snprintk(line, sizeof(line), "PASS %s", what);
		passes++;
	} else {
		(void)snprintk(line, sizeof(line), "FAIL %s (rc=%d)", what, rc);
		fails++;
	}
	app_svc_log(line);
}

static void owner_app(void *arg)
{
	ARG_UNUSED(arg);

	uint32_t v = 0;
	int rc = app_svc_kv_set("count", 41);

	check(rc == 0, "set a key", rc);

	rc = app_svc_kv_get("count", &v);
	check(rc == 0 && v == 41U, "read it back", rc == 0 ? (int)v : rc);

	rc = app_svc_kv_set("count", 42);
	check(rc == 0, "overwrite it", rc);
	rc = app_svc_kv_get("count", &v);
	check(rc == 0 && v == 42U, "the overwrite took", rc == 0 ? (int)v : rc);

	/* A second key so the delete below is shown to remove one entry
	 * rather than clearing the application's whole namespace.
	 */
	rc = app_svc_kv_set("keep", 7);
	check(rc == 0, "set a second key", rc);

	rc = app_svc_kv_delete("count");
	check(rc == 0, "delete the first key", rc);

	rc = app_svc_kv_get("count", &v);
	check(rc == -ENOENT, "the deleted key is gone", rc);

	rc = app_svc_kv_get("keep", &v);
	check(rc == 0 && v == 7U, "the other key survived the delete",
	      rc == 0 ? (int)v : rc);

	rc = app_svc_kv_delete("count");
	check(rc == -ENOENT, "deleting it again reports ENOENT", rc);

	rc = app_svc_kv_delete("never-set");
	check(rc == -ENOENT, "deleting an unknown key reports ENOENT", rc);

	/* The slot freed by the delete must be reusable, which is the point of
	 * having a delete at all in a fixed-size store.
	 */
	rc = app_svc_kv_set("count", 99);
	check(rc == 0, "the freed slot takes a new key", rc);
	rc = app_svc_kv_get("count", &v);
	check(rc == 0 && v == 99U, "the new key reads back",
	      rc == 0 ? (int)v : rc);

	/* Leave a value the other application will look for under the same
	 * name, so the namespacing check below means something.
	 */
	(void)app_svc_kv_set("shared", 1111);
	app_svc_log("kv_store: owner done");
}

static void other_app(void *arg)
{
	ARG_UNUSED(arg);

	uint32_t v = 0;

	/* Same key name, different application: the store is keyed by owner,
	 * so this must not see the other application's value.
	 */
	int rc = app_svc_kv_get("shared", &v);

	check(rc == -ENOENT, "another app's key is not visible", rc);

	rc = app_svc_kv_delete("shared");
	check(rc == -ENOENT, "another app's key cannot be deleted", rc);

	rc = app_svc_kv_set("shared", 2222);
	check(rc == 0, "the same name is usable in this app", rc);
	rc = app_svc_kv_get("shared", &v);
	check(rc == 0 && v == 2222U, "and holds this app's own value",
	      rc == 0 ? (int)v : rc);

	char line[48];

	(void)snprintk(line, sizeof(line), "kv_store: %d passed, %d failed",
		       passes, fails);
	app_svc_log(line);
	app_svc_log(fails == 0 ? "kv_store: done" : "kv_store: FAILURES");
}

int main(void)
{
	printk("\n*** app_kv_store: per-application key/value storage ***\n");

	if (app_platform_init() != 0) {
		printk("platform init failed\n");
		return 0;
	}

	struct app_def owner = {
		.name = "kvowner",
		.source = APP_BUILTIN,
		.entry = owner_app,
		.caps = APP_CAP_LOG | APP_CAP_KV,
		.stopped = true,
	};
	struct app_def other = {
		.name = "kvother",
		.source = APP_BUILTIN,
		.entry = other_app,
		.caps = APP_CAP_LOG | APP_CAP_KV,
		.stopped = true,
	};

	int a = app_platform_launch(&owner);

	if (a < 0) {
		printk("launch failed (%d)\n", a);
		return 0;
	}
	k_mem_domain_add_partition(app_platform_domain(a), &kv_part);
	(void)app_platform_start(a);

	/* Let the first application finish writing before the second looks for
	 * the same key name, so the namespacing check is not a race.
	 */
	k_sleep(K_MSEC(200));

	int b = app_platform_launch(&other);

	if (b < 0) {
		printk("second launch failed (%d)\n", b);
		return 0;
	}
	k_mem_domain_add_partition(app_platform_domain(b), &kv_part);
	(void)app_platform_start(b);

	printk("*** launched kv apps (groups %d and %d) ***\n", a, b);
	return 0;
}
