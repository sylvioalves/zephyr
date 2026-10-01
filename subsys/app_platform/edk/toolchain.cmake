# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# CMake toolchain file of the application module development kit. It selects
# the compiler of the toolchain the firmware was built with, which the kit does
# not hold: APP_EDK_CROSS_COMPILE is the path prefix of its tools, such as
# /opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-, or else the tools
# are taken from the Zephyr SDK at ZEPHYR_SDK_INSTALL_DIR. Both may also be
# given in the environment.

get_filename_component(APP_EDK_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
include(${APP_EDK_DIR}/cmake/app_edk_target.cmake)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES APP_EDK_CROSS_COMPILE)

if(NOT DEFINED APP_EDK_CROSS_COMPILE AND DEFINED ENV{APP_EDK_CROSS_COMPILE})
  set(APP_EDK_CROSS_COMPILE $ENV{APP_EDK_CROSS_COMPILE})
endif()
if(NOT DEFINED APP_EDK_CROSS_COMPILE)
  if(NOT DEFINED ZEPHYR_SDK_INSTALL_DIR AND DEFINED ENV{ZEPHYR_SDK_INSTALL_DIR})
    set(ZEPHYR_SDK_INSTALL_DIR $ENV{ZEPHYR_SDK_INSTALL_DIR})
  endif()
  if(DEFINED ZEPHYR_SDK_INSTALL_DIR AND NOT "${APP_EDK_SDK_BIN_DIR}" STREQUAL "")
    set(APP_EDK_CROSS_COMPILE
      ${ZEPHYR_SDK_INSTALL_DIR}/${APP_EDK_SDK_BIN_DIR}/${APP_EDK_TOOL_PREFIX})
  else()
    message(FATAL_ERROR
      "No toolchain: set APP_EDK_CROSS_COMPILE to the path prefix of the "
      "${APP_EDK_TRIPLE} tools (<dir>/${APP_EDK_TOOL_PREFIX}), or "
      "ZEPHYR_SDK_INSTALL_DIR to Zephyr SDK ${APP_EDK_SDK_VERSION}")
  endif()
endif()
set(APP_EDK_CROSS_COMPILE ${APP_EDK_CROSS_COMPILE} CACHE STRING
  "Path prefix of the toolchain tools")

set(CMAKE_C_COMPILER ${APP_EDK_CROSS_COMPILE}gcc)
set(CMAKE_OBJCOPY ${APP_EDK_CROSS_COMPILE}objcopy CACHE FILEPATH "")
set(CMAKE_NM ${APP_EDK_CROSS_COMPILE}nm CACHE FILEPATH "")
if(NOT EXISTS ${CMAKE_C_COMPILER})
  message(FATAL_ERROR "${CMAKE_C_COMPILER} does not exist")
endif()
