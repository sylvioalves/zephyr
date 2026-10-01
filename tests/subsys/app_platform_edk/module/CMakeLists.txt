# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# A module project of a writer who has the development kit of the firmware,
# APP_EDK_DIR, its toolchain and a signing key, but not the Zephyr tree.

cmake_minimum_required(VERSION 3.20.0)

if(DEFINED ENV{ZEPHYR_BASE})
  message(FATAL_ERROR "the module project must build without ZEPHYR_BASE")
endif()

include(${APP_EDK_DIR}/cmake/app_edk.cmake)
project(edk_modules LANGUAGES C)

app_edk_add_module(hello
  NAME edk_hello
  VERSION 1
  SOURCES hello.c
  REQUESTED_CAPS LOG,TIME
)

# The same module signed with a key the firmware does not list, which the
# signer would refuse.
set(unlisted_key "${CMAKE_CURRENT_BINARY_DIR}/unlisted.pem")
string(CONCAT new_key
  "import sys; sys.path.insert(0, sys.argv[1]); import gen_app_keys; "
  "gen_app_keys.create_private_key(sys.argv[2])"
)
add_custom_command(
  OUTPUT ${unlisted_key}
  COMMAND ${Python3_EXECUTABLE} -c "${new_key}" ${APP_EDK_DIR}/scripts ${unlisted_key}
  VERBATIM
)
app_edk_add_module(unlisted
  NAME edk_unlisted
  VERSION 1
  SOURCES hello.c
  REQUESTED_CAPS LOG,TIME
  SIGNING_KEY ${unlisted_key}
  SIGNING_KEY_ID 7
  SKIP_KEY_CHECK
)

# Modules the kit refuses to build: one including kernel.h and one importing a
# kernel function.
set(project_args
  -DAPP_EDK_DIR=${APP_EDK_DIR}
  -DAPP_EDK_CROSS_COMPILE=${APP_EDK_CROSS_COMPILE}
  -DPython3_EXECUTABLE=${Python3_EXECUTABLE}
  -DAPP_SIGNING_KEY=${APP_SIGNING_KEY}
  -DAPP_SIGNING_KEY_ID=${APP_SIGNING_KEY_ID}
)
function(expect_refused name expect)
  add_custom_command(
    OUTPUT ${name}.refused
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/${name}
      -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/${name}
      -DGENERATOR=${CMAKE_GENERATOR}
      -DEXPECT=${expect}
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/${name}.refused
      "-DARGS=${project_args}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_failure.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${name}/${name}.c
      ${CMAKE_CURRENT_SOURCE_DIR}/expect_failure.cmake
    VERBATIM
  )
endfunction()

expect_refused(kernel_h "zephyr/kernel.h: No such file")
expect_refused(forbidden "may not use: k_sched_lock")
add_custom_target(refused ALL DEPENDS kernel_h.refused forbidden.refused)
