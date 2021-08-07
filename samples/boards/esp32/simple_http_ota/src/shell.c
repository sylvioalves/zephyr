/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <shell/shell.h>
#include <drivers/flash.h>
#include <storage/flash_map.h>
#include <dfu/mcuboot.h>
#include <dfu/flash_img.h>
#include <sys/reboot.h>
#include <string.h>
#include <drivers/hwinfo.h>

#include "simple_http_ota.h"

#define DEVICE_ID_BIN_MAX_SIZE	16
#define DEVICE_ID_HEX_MAX_SIZE	((DEVICE_ID_BIN_MAX_SIZE * 2) + 1)

static void cmd_run(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_fprintf(shell, SHELL_INFO, "Starting SIMPLE HTTP OTA...\n");
	simple_http_ota_run();
	k_sleep(K_MSEC(1));
}

static void esp_get_firmware_version(char *version, int version_len)
{
	snprintk(version, version_len, "%d.%d",
		SIMPLE_HTTP_OTA_MAJOR_VERSION, SIMPLE_HTTP_OTA_MINOR_VERSION);
}

bool esp_get_device_identity(char *id, int id_max_len)
{
	uint8_t hwinfo_id[DEVICE_ID_BIN_MAX_SIZE];
	ssize_t length;

	length = hwinfo_get_device_id(hwinfo_id, DEVICE_ID_BIN_MAX_SIZE);
	if (length <= 0) {
		return false;
	}

	memset(id, 0, id_max_len);
	length = bin2hex(hwinfo_id, (size_t)length, id, id_max_len - 1);

	return length > 0;
}

static int cmd_info(const struct shell *shell, size_t argc, char *argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char device_id[DEVICE_ID_HEX_MAX_SIZE] = {0};
	char firmware_version[BOOT_IMG_VER_STRLEN_MAX] = {0};

	esp_get_firmware_version(firmware_version, BOOT_IMG_VER_STRLEN_MAX);
	esp_get_device_identity(device_id, DEVICE_ID_HEX_MAX_SIZE);

	shell_fprintf(shell, SHELL_NORMAL, "Device macaddress: %s\n", device_id);
	shell_fprintf(shell, SHELL_NORMAL, "Firmware Version: %s\n", firmware_version);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	simple_http_ota,
	SHELL_CMD(info, NULL, "Display board information", cmd_info),
	SHELL_CMD(run, NULL, "Start firmware update", cmd_run),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &simple_http_ota, "Shell commands", NULL);
