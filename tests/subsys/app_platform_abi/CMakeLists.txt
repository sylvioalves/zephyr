# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_platform_abi_test)

target_sources(app PRIVATE src/main.c src/renumber_probe.c)

# The module the producer image, another firmware for the same target, built
# and signed, and the facts of that firmware.
set(producer_dir ${CMAKE_BINARY_DIR}/../app_abi_producer/zephyr/include/generated)
set(copy_dir ${ZEPHYR_BINARY_DIR}/include/generated/producer)
foreach(name renumber_mod.inc renumber_mod.manifest.inc renumber_mod.sig.inc
             renumber_facts.h)
  list(APPEND producer_files ${producer_dir}/${name})
  list(APPEND copied_files ${copy_dir}/${name})
endforeach()

add_custom_command(
  OUTPUT ${copied_files}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${copy_dir}
  COMMAND ${CMAKE_COMMAND} -E copy ${producer_files} ${copy_dir}
  DEPENDS ${producer_files}
)
add_custom_target(producer_files DEPENDS ${copied_files})
add_dependencies(app producer_files)
