# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_display)

target_sources(app PRIVATE
  src/main.c
)
target_include_directories(app PRIVATE common)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/display_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/display_app.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/display_app.inc)
set(sha ${ZEPHYR_BINARY_DIR}/include/generated/display_app.sha256.inc)

app_platform_add_module(display_app_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(display_app_ext
  ${ZEPHYR_BINARY_DIR}/include/generated
  ${PROJECT_SOURCE_DIR}/common
)
generate_inc_file_for_target(app ${bin} ${inc})

add_custom_command(
  OUTPUT  ${sha}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DOUT_INC=${sha}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_sha256_inc.cmake
  DEPENDS ${bin}
)
add_custom_target(display_app_sha DEPENDS ${sha})
add_dependencies(app display_app_sha)
