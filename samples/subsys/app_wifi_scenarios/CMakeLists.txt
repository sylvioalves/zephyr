# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_wifi_scenarios)

target_sources(app PRIVATE src/main.c)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/net_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/net_app.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/net_app.inc)
set(sha ${ZEPHYR_BINARY_DIR}/include/generated/net_app.sha256.inc)

add_llext_target(net_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(net_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

add_custom_command(
  OUTPUT  ${sha}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DOUT_INC=${sha}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_sha256_inc.cmake
  DEPENDS ${bin}
)
add_custom_target(net_sha DEPENDS ${sha})
add_dependencies(app net_sha)
