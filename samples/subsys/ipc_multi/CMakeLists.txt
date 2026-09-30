# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ipc_multi)

target_sources(app PRIVATE
  src/main.c
)

# Build the one module three times as three roles, each a verified llext.
set(ext_src ${PROJECT_SOURCE_DIR}/module/src/ipc_app.c)

function(build_role name role)
  set(bin ${PROJECT_BINARY_DIR}/llext/${name}.llext)
  set(inc ${ZEPHYR_BINARY_DIR}/include/generated/${name}.inc)
  set(sha ${ZEPHYR_BINARY_DIR}/include/generated/${name}.sha256.inc)

  app_platform_add_module(${name}_ext OUTPUT ${bin} SOURCES ${ext_src})
  llext_compile_definitions(${name}_ext ROLE=${role})
  llext_include_directories(${name}_ext
    ${ZEPHYR_BINARY_DIR}/include/generated
  )
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

build_role(producer 1)
build_role(consumer 2)
build_role(logger 3)
