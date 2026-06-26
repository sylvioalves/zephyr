# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(platform)

zephyr_syscall_header(${CMAKE_CURRENT_SOURCE_DIR}/src/svc.h)

target_sources(app PRIVATE
	src/main.c
	src/manager.c
	src/svc.c
	src/apps.c
	src/app_registry.c
	src/loader.c
)

# The downloadable app, built as an llext. It calls the platform service
# syscalls, so it needs the service header and the generated syscall headers.
set(ext_name installed_app)
set(ext_src module/src/${ext_name}.c)
set(ext_bin ${PROJECT_BINARY_DIR}/llext/${ext_name}.llext)
set(ext_inc ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.inc)
set(ext_sha ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.sha256.inc)

add_llext_target(${ext_name}_ext
  OUTPUT  ${ext_bin}
  SOURCES ${ext_src}
)
llext_include_directories(${ext_name}_ext
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${ZEPHYR_BINARY_DIR}/include/generated
)

generate_inc_file_for_target(app ${ext_bin} ${ext_inc})

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
