/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Console control and introspection for the application platform: a live view
 * of installed and running applications and their resource use, plus lifecycle
 * control (start/stop/restart/remove). Queryable over any shell backend
 * (serial, or the mcumgr/network shell for remote access).
 */

#include <zephyr/app_platform/app_platform.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/kernel.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_APP_PLATFORM_REGISTRY
#include <zephyr/app_platform/app_registry.h>
#endif

extern struct k_heap _system_heap;

static void print_running(const struct shell *sh)
{
	int n = app_platform_count();
	bool any = false;
	size_t llext_used = 0;

	shell_print(sh, "running apps:");
	shell_print(sh, "  %-3s %-16s %-8s %-13s %-10s %s", "id", "name",
		    "state", "stack(used/sz)", "module", "cpu%");

	for (int i = 0; i < n; i++) {
		struct app_platform_status s;

		if (app_platform_status(i, &s) != 0) {
			continue;
		}
		any = true;
		llext_used += s.module_bytes;

		unsigned int cpu = 0;

		if (s.cpu_total > 0) {
			cpu = (unsigned int)((s.cpu_cycles * 100U) / s.cpu_total);
		}

		shell_print(sh, "  %-3d %-16s %-8s %6zu/%-6zu %8zu  %3u%%", i,
			    s.name ? s.name : "?",
			    s.faulted ? "faulted" : (s.active ? "running" : "stopped"),
			    s.stack_used, s.stack_size, s.module_bytes, cpu);
	}
	if (!any) {
		shell_print(sh, "  (none)");
	}
	shell_print(sh, "loadable apps use %zu bytes total", llext_used);
}

#ifdef CONFIG_APP_PLATFORM_REGISTRY
static void print_installed(const struct shell *sh)
{
	int n = app_registry_count();

	shell_print(sh, "installed apps:");
	if (n == 0) {
		shell_print(sh, "  (none)");
		return;
	}
	for (int i = 0; i < n; i++) {
		struct app_record r;

		if (app_registry_get(i, &r) == 0) {
			shell_print(sh, "  %-16s v%-4u %s", r.name, r.version,
				    r.autostart ? "autostart" : "");
		}
	}
}
#endif

static void print_heap(const struct shell *sh)
{
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
	struct sys_memory_stats st;

	if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) == 0) {
		shell_print(sh, "system heap: %zu used / %zu free (peak %zu)",
			    st.allocated_bytes, st.free_bytes,
			    st.max_allocated_bytes);
	}
#else
	shell_print(sh, "system heap: enable CONFIG_SYS_HEAP_RUNTIME_STATS for stats");
#endif
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#ifdef CONFIG_APP_PLATFORM_REGISTRY
	print_installed(sh);
	shell_print(sh, "");
#endif
	print_running(sh);
	shell_print(sh, "");
	print_heap(sh);
	return 0;
}

/* Lifecycle commands take a group id. */
static int parse_id(const struct shell *sh, const char *arg, int *id)
{
	char *end;
	long v = strtol(arg, &end, 10);

	if (*end != '\0' || v < 0 || v >= app_platform_count()) {
		shell_error(sh, "bad id '%s'", arg);
		return -EINVAL;
	}
	*id = (int)v;
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	int id;

	if (parse_id(sh, argv[1], &id) != 0) {
		return -EINVAL;
	}
	int rc = app_platform_stop(id);

	shell_print(sh, rc == 0 ? "stopped app %d" : "stop failed (%d)",
		    rc == 0 ? id : rc);
	return rc;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	int id;

	if (parse_id(sh, argv[1], &id) != 0) {
		return -EINVAL;
	}
	int rc = app_platform_start(id);

	shell_print(sh, rc >= 0 ? "started app %d" : "start failed (%d)",
		    rc >= 0 ? id : rc);
	return rc < 0 ? rc : 0;
}

static int cmd_restart(const struct shell *sh, size_t argc, char **argv)
{
	int id;

	if (parse_id(sh, argv[1], &id) != 0) {
		return -EINVAL;
	}
	int rc = app_platform_restart(id);

	shell_print(sh, rc >= 0 ? "restarted app %d" : "restart failed (%d)",
		    rc >= 0 ? id : rc);
	return rc < 0 ? rc : 0;
}

static int cmd_remove(const struct shell *sh, size_t argc, char **argv)
{
	int id;

	if (parse_id(sh, argv[1], &id) != 0) {
		return -EINVAL;
	}
	int rc = app_platform_remove(id);

	shell_print(sh, rc == 0 ? "removed app %d" : "remove failed (%d)",
		    rc == 0 ? id : rc);
	return rc;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "on") == 0) {
		app_platform_log_enable(true);
	} else if (strcmp(argv[1], "off") == 0) {
		app_platform_log_enable(false);
	} else {
		shell_error(sh, "usage: app log <on|off>");
		return -EINVAL;
	}
	shell_print(sh, "app log %s", app_platform_log_is_enabled() ? "on" : "off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(app_cmds,
	SHELL_CMD(status, NULL, "installed + running apps, stacks, cpu, heap",
		  cmd_status),
	SHELL_CMD_ARG(stop, NULL, "stop <id>: pause an app (keeps it loaded)",
		      cmd_stop, 2, 0),
	SHELL_CMD_ARG(start, NULL, "start <id>: resume a stopped app",
		      cmd_start, 2, 0),
	SHELL_CMD_ARG(restart, NULL, "restart <id>: stop then start an app",
		      cmd_restart, 2, 0),
	SHELL_CMD_ARG(remove, NULL, "remove <id>: stop and unload an app",
		      cmd_remove, 2, 0),
	SHELL_CMD_ARG(log, NULL, "log <on|off>: show or mute app log output",
		      cmd_log, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(app, &app_cmds, "App platform control and introspection",
		   NULL);
