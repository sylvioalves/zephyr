# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER_FORCED TRUE)
project(extension LANGUAGES C)

include(${LLEXT_EDK_INSTALL_DIR}/cmake.cflags)
foreach(module IN LISTS LLEXT_CMAKE_MODULES)
  include(${module})
endforeach()

if(NOT EXISTS ${LLEXT_EDK_INSTALL_DIR}/doc/README.txt)
  message(FATAL_ERROR "The EDK lacks the file added by the application")
endif()

profile_add_extension(extension src/main.c)
