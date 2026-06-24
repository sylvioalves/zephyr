# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_store)

set(ext_name blink_app)
set(ext_src module/src/${ext_name}.c)
set(ext_bin ${PROJECT_BINARY_DIR}/llext/${ext_name}.llext)
set(ext_inc ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.inc)
set(ext_sha ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.sha256.inc)

# Build the downloadable module as an llext.
add_llext_target(${ext_name}_ext
  OUTPUT  ${ext_bin}
  SOURCES ${ext_src}
)

# Embed the module bytes as a C array.
generate_inc_file_for_target(app ${ext_bin} ${ext_inc})

# Compute the module's SHA-256 at build time and emit it as a C array, so the
# app has the expected hash to verify against (the store's "signature").
add_custom_command(
  OUTPUT  ${ext_sha}
  COMMAND ${CMAKE_COMMAND}
          -DMODULE_BIN=${ext_bin}
          -DOUT_INC=${ext_sha}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_sha256_inc.cmake
  DEPENDS ${ext_bin}
)
add_custom_target(${ext_name}_sha DEPENDS ${ext_sha})
add_dependencies(app ${ext_name}_sha)

target_sources(app PRIVATE src/main.c)
