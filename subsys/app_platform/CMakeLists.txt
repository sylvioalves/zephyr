# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(app_manager.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_LOADER   app_loader.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_REGISTRY app_registry.c)

zephyr_library_link_libraries_ifdef(CONFIG_FILE_SYSTEM_LITTLEFS LITTLEFS)
