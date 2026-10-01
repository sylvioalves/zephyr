# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# A module project built with the module development kit of the firmware,
# outside the Zephyr tree:
#
#   cmake -S module -B build -G Ninja -DAPP_EDK_DIR=<kit> \
#         -DAPP_EDK_CROSS_COMPILE=<toolchain prefix> \
#         -DAPP_SIGNING_KEY=<publisher.pem> -DAPP_SIGNING_KEY_ID=1
#   cmake --build build

cmake_minimum_required(VERSION 3.20.0)
include(${APP_EDK_DIR}/cmake/app_edk.cmake)
project(hello LANGUAGES C)

app_edk_add_module(hello
  NAME hello
  VERSION 1
  SOURCES hello.c
  REQUESTED_CAPS LOG,TIME
)
