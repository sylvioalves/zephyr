# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
include(${APP_EDK_DIR}/cmake/app_edk.cmake)
project(forbidden LANGUAGES C)

app_edk_add_module(forbidden NAME forbidden VERSION 1 SOURCES forbidden.c REQUESTED_CAPS LOG)
