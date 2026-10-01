# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_platform_edk_test)

target_sources(app PRIVATE src/main.c)
if(CONFIG_APPLICATION_DEFINED_SYSCALL)
  target_sources(app PRIVATE src/edk_probe.c)
endif()

# The modules of the module project, built and signed with the development kit
# of this firmware by a writer holding the key listed under key id 1.
set(writer_key ${CMAKE_CURRENT_SOURCE_DIR}/keys/writer.pem)
app_platform_edk_project(edk_modules
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/module
  OUTPUTS hello.llext hello.manifest unlisted.llext unlisted.manifest
  DEPENDS ${writer_key}
  CMAKE_ARGS -DAPP_SIGNING_KEY=${writer_key} -DAPP_SIGNING_KEY_ID=1
)

# The test image also loads the modules built with the kit of the other
# firmware image, edk_peer, and compares the two firmwares. The other image
# uses its own modules and facts in their place.
set(own_dir ${CMAKE_CURRENT_BINARY_DIR}/edk_modules/build)
set(own_gen ${ZEPHYR_BINARY_DIR}/include/generated/zephyr)
if(SYSBUILD_MAIN_APP)
  set(peer_dir ${CMAKE_BINARY_DIR}/../edk_peer/edk_modules/build)
  set(peer_gen ${CMAKE_BINARY_DIR}/../edk_peer/zephyr/include/generated/zephyr)
else()
  set(peer_dir ${own_dir})
  set(peer_gen ${own_gen})
endif()

# The tick rate and a syscall number of each firmware.
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/edk)
foreach(fw own peer)
  string(TOUPPER ${fw} prefix)
  add_custom_command(
    OUTPUT ${gen_dir}/${fw}_facts.h
    COMMAND ${CMAKE_COMMAND}
      -DSYSCALL_LIST=${${fw}_gen}/syscall_list.h
      -DAUTOCONF=${${fw}_gen}/autoconf.h
      -DPREFIX=${prefix}
      -DOUT=${gen_dir}/${fw}_facts.h
      -P ${CMAKE_CURRENT_SOURCE_DIR}/facts.cmake
    DEPENDS ${${fw}_gen}/syscall_list.h ${${fw}_gen}/autoconf.h
      ${CMAKE_CURRENT_SOURCE_DIR}/facts.cmake
  )
  add_custom_target(edk_${fw}_facts DEPENDS ${gen_dir}/${fw}_facts.h)
  add_dependencies(edk_${fw}_facts ${SYSCALL_LIST_H_TARGET})
  add_dependencies(app edk_${fw}_facts)
endforeach()

foreach(file hello.llext hello.manifest unlisted.llext unlisted.manifest)
  generate_inc_file_for_target(app ${own_dir}/${file} ${gen_dir}/own_${file}.inc)
endforeach()
foreach(file hello.llext hello.manifest)
  generate_inc_file_for_target(app ${peer_dir}/${file} ${gen_dir}/peer_${file}.inc)
endforeach()
add_dependencies(app edk_modules)
