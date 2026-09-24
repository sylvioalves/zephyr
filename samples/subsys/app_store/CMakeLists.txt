# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_store)

# Identity bound into the signed manifest.
set(APP_NAME    "store")
set(APP_VERSION 1)

set(ext_name blink_app)
set(ext_src module/src/${ext_name}.c)
set(ext_bin ${PROJECT_BINARY_DIR}/llext/${ext_name}.llext)
set(ext_inc ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.inc)
set(ext_mf  ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.manifest.inc)
set(ext_sig ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.sig.inc)
set(ext_pub ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}.pubkey.inc)

# Build the downloadable module as an llext.
add_llext_target(${ext_name}_ext OUTPUT ${ext_bin} SOURCES ${ext_src})

# Embed the module bytes as a C array.
generate_inc_file_for_target(app ${ext_bin} ${ext_inc})

# Build and sign a manifest binding the name, version and module hash, then
# embed the manifest, signature and trusted public key. The private key is
# generated once under the source tree and never compiled in.
add_custom_command(
  OUTPUT  ${ext_mf} ${ext_sig} ${ext_pub}
  COMMAND ${CMAKE_COMMAND}
          -DMODULE_BIN=${ext_bin}
          -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
          -DAPP_NAME=${APP_NAME}
          -DAPP_VERSION=${APP_VERSION}
          -DABI_FILE=${APP_PLATFORM_ABI_FILE}
          -DOUT_MF_INC=${ext_mf}
          -DOUT_SIG_INC=${ext_sig}
          -DOUT_PUB_INC=${ext_pub}
          -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
          -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${ext_bin} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(${ext_name}_sign DEPENDS ${ext_mf} ${ext_sig} ${ext_pub})
add_dependencies(${ext_name}_sign app_platform_abi)
add_dependencies(app ${ext_name}_sign)

target_sources(app PRIVATE src/main.c)
