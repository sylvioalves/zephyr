# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_edk)

target_sources(app PRIVATE src/main.c)

# The firmware runs the module bundle (hello.llext and hello.manifest) found in
# APP_EDK_BUNDLE_DIR, built outside the tree from the kit of this firmware. Without
# it, the build builds module/ with its own kit and the publisher key.
zephyr_get(APP_EDK_BUNDLE_DIR)
if(APP_EDK_BUNDLE_DIR)
  get_filename_component(bundle_dir ${APP_EDK_BUNDLE_DIR} ABSOLUTE)
else()
  set(publisher_key ${CMAKE_CURRENT_SOURCE_DIR}/keys/publisher.pem)
  app_platform_edk_project(hello_module
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/module
    OUTPUTS hello.llext hello.manifest
    DEPENDS ${publisher_key}
    CMAKE_ARGS -DAPP_SIGNING_KEY=${publisher_key} -DAPP_SIGNING_KEY_ID=1
  )
  add_dependencies(app hello_module)
  set(bundle_dir ${CMAKE_CURRENT_BINARY_DIR}/hello_module/build)
endif()

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/app_edk)
generate_inc_file_for_target(app ${bundle_dir}/hello.llext ${gen_dir}/hello.llext.inc)
generate_inc_file_for_target(app ${bundle_dir}/hello.manifest ${gen_dir}/hello.manifest.inc)
