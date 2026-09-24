# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_scale)

target_sources(app PRIVATE src/main.c)

set(ext_src ${PROJECT_SOURCE_DIR}/../app_top/module/src/loop_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/loop_app.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/loop_app.inc)
set(sha ${ZEPHYR_BINARY_DIR}/include/generated/loop_app.sha256.inc)

add_llext_target(loop_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(loop_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

add_custom_command(
  OUTPUT  ${sha}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DOUT_INC=${sha}
    -P ${PROJECT_SOURCE_DIR}/../app_top/cmake/gen_sha256_inc.cmake
  DEPENDS ${bin}
)
add_custom_target(loop_sha DEPENDS ${sha})
add_dependencies(app loop_sha)
