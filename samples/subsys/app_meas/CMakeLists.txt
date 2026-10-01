# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_meas)

target_sources(app PRIVATE src/main.c)

function(add_sized_module name pad)
  set(bin ${PROJECT_BINARY_DIR}/llext/${name}.llext)
  set(inc ${ZEPHYR_BINARY_DIR}/include/generated/${name}.inc)
  set(mf  ${ZEPHYR_BINARY_DIR}/include/generated/${name}.manifest.inc)
  set(sig ${ZEPHYR_BINARY_DIR}/include/generated/${name}.sig.inc)

  app_platform_add_module(${name}_ext OUTPUT ${bin}
    SOURCES ${PROJECT_SOURCE_DIR}/module/src/pad_app.c)
  llext_compile_definitions(${name}_ext PAD_BYTES=${pad})
  llext_include_directories(${name}_ext ${ZEPHYR_BINARY_DIR}/include/generated)
  generate_inc_file_for_target(app ${bin} ${inc})

  add_custom_command(
    OUTPUT  ${mf} ${sig}
    COMMAND ${CMAKE_COMMAND}
      -DMODULE_BIN=${bin}
      -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
      -DAPP_NAME=${name}
      -DAPP_VERSION=1
      -DAPP_REQUESTED_CAPS=LOG
      -DAPP_REQUIRED_CAPS=LOG
      -DABI_FILE=${APP_PLATFORM_ABI_FILE}
      -DOUT_MF_INC=${mf}
      -DOUT_SIG_INC=${sig}
      -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
      -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
    DEPENDS ${bin} ${APP_PLATFORM_ABI_FILE}
  )
  add_custom_target(${name}_sign DEPENDS ${mf} ${sig})
  add_dependencies(${name}_sign app_platform_abi)
  add_dependencies(app ${name}_sign)

  set(sha ${ZEPHYR_BINARY_DIR}/include/generated/${name}.sha256.inc)
  add_custom_command(
    OUTPUT  ${sha}
    COMMAND ${CMAKE_COMMAND} -DMODULE_BIN=${bin} -DOUT_INC=${sha}
      -P ${PROJECT_SOURCE_DIR}/../app_top/cmake/gen_sha256_inc.cmake
    DEPENDS ${bin}
  )
  add_custom_target(${name}_sha DEPENDS ${sha})
  add_dependencies(app ${name}_sha)
endfunction()

add_sized_module(meas0 0)
add_sized_module(meas4 4096)
add_sized_module(meas16 16384)
add_sized_module(meas64 65536)
