# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
include(${APP_EDK_DIR}/cmake/app_edk.cmake)
project(kernel_h LANGUAGES C)

app_edk_add_module(kernel_h NAME kernel_h VERSION 1 SOURCES kernel_h.c REQUESTED_CAPS LOG)
