# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_sdk)

# Build the loadable app as an llext. It only needs the SDK header.
set(app_bin ${PROJECT_BINARY_DIR}/llext/demo_app.llext)
set(app_inc ${ZEPHYR_BINARY_DIR}/include/generated/demo_app.inc)

add_llext_target(demo_app_ext
  OUTPUT  ${app_bin}
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/module/src/demo_app.c
)
# The module needs to find app_sdk.h.
llext_include_directories(demo_app_ext
  ${CMAKE_CURRENT_SOURCE_DIR}/src)

generate_inc_file_for_target(app ${app_bin} ${app_inc})

target_sources(app PRIVATE
  src/main.c
  src/app_sdk.c
  src/platform.c
)
