# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_trio)

target_sources(app PRIVATE src/main.c)

# Build each of the three modules as an llext and embed its bytes + hash.
function(add_module name)
  set(src ${PROJECT_SOURCE_DIR}/module/src/${name}.c)
  set(bin ${PROJECT_BINARY_DIR}/llext/${name}.llext)
  set(inc ${ZEPHYR_BINARY_DIR}/include/generated/${name}.inc)
  set(sha ${ZEPHYR_BINARY_DIR}/include/generated/${name}.sha256.inc)

  add_llext_target(${name}_ext OUTPUT ${bin} SOURCES ${src})
  llext_include_directories(${name}_ext ${ZEPHYR_BINARY_DIR}/include/generated)
  generate_inc_file_for_target(app ${bin} ${inc})

  add_custom_command(
    OUTPUT  ${sha}
    COMMAND ${CMAKE_COMMAND}
      -DMODULE_BIN=${bin}
      -DOUT_INC=${sha}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_sha256_inc.cmake
    DEPENDS ${bin}
  )
  add_custom_target(${name}_sha DEPENDS ${sha})
  add_dependencies(app ${name}_sha)
endfunction()

add_module(pub_app)
add_module(bridge_app)
add_module(print_app)
