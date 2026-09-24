# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_fs_sandbox)

zephyr_syscall_header(${CMAKE_CURRENT_SOURCE_DIR}/src/fs_svc.h)

target_sources(app PRIVATE
  src/main.c
  src/fs_broker.c
  src/fs_svc.c
)
