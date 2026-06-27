# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(app_manager.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_LOADER   app_loader.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_REGISTRY app_registry.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_SERVICES app_services.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_NET      app_net.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_GPIO     app_gpio.c)

if(CONFIG_APP_PLATFORM_SERVICES)
  zephyr_syscall_header(${ZEPHYR_BASE}/include/zephyr/app_platform/app_services.h)
endif()

zephyr_library_link_libraries_ifdef(CONFIG_FILE_SYSTEM_LITTLEFS LITTLEFS)
