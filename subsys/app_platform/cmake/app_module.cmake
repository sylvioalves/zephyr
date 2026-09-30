# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# app_platform_add_module(<target> <add_llext_target arguments>...)
#
# Builds a loadable application module like add_llext_target(), with
# APP_PLATFORM_MODULE defined so that its sources build against
# <zephyr/app_platform/app_sdk.h> and cannot include app_services.h.
function(app_platform_add_module target_name)
  add_llext_target(${target_name} ${ARGN})
  llext_compile_definitions(${target_name} APP_PLATFORM_MODULE)
endfunction()
