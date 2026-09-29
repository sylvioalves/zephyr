# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_platform_devices)

target_sources(app PRIVATE src/main.c src/display_capture.c src/locked_svc.c)
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/subsys/app_platform)
