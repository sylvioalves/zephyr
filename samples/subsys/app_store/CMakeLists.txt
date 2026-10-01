# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_store)

# Identity bound into the signed manifest.
set(APP_NAME    "store")
set(APP_VERSION 1)
set(APP_REQUESTED_CAPS LOG)
set(APP_REQUIRED_CAPS  LOG)

set(ext_name blink_app)
set(ext_src module/src/${ext_name}.c)
set(ext_bin ${PROJECT_BINARY_DIR}/llext/${ext_name}.llext)
set(ext_inc ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.inc)
set(ext_mf  ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.manifest.inc)
set(ext_sig ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.sig.inc)

# Build the downloadable module as an llext.
app_platform_add_module(${ext_name}_ext OUTPUT ${ext_bin} SOURCES ${ext_src})

# Embed the module bytes as a C array.
generate_inc_file_for_target(app ${ext_bin} ${ext_inc})

# Build and sign a manifest binding the name, version and module hash, then
# embed the manifest and signature. The private key is generated once under the
# source tree and never compiled in.
add_custom_command(
  OUTPUT  ${ext_mf} ${ext_sig}
  COMMAND ${CMAKE_COMMAND}
          -DMODULE_BIN=${ext_bin}
          -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
          -DAPP_NAME=${APP_NAME}
          -DAPP_VERSION=${APP_VERSION}
          -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
          -DAPP_REQUIRED_CAPS=${APP_REQUIRED_CAPS}
          -DABI_FILE=${APP_PLATFORM_ABI_FILE}
          -DOUT_MF_INC=${ext_mf}
          -DOUT_SIG_INC=${ext_sig}
          -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
          -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${ext_bin} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(${ext_name}_sign DEPENDS ${ext_mf} ${ext_sig})
add_dependencies(${ext_name}_sign app_platform_abi)
add_dependencies(app ${ext_name}_sign)

target_sources(app PRIVATE src/main.c)
