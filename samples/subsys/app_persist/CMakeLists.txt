# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_persist)

# Identity bound into the signed manifest; keep in sync with src/main.c.
set(APP_NAME    "persist")
set(APP_VERSION 1)

target_sources(app PRIVATE src/main.c)
target_compile_definitions(app PRIVATE
  APP_NAME="${APP_NAME}" APP_VERSION=${APP_VERSION})

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/persist_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/persist.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/persist.inc)
set(mf  ${ZEPHYR_BINARY_DIR}/include/generated/persist.manifest.inc)
set(sig ${ZEPHYR_BINARY_DIR}/include/generated/persist.sig.inc)
set(pub ${ZEPHYR_BINARY_DIR}/include/generated/persist.pubkey.inc)

add_llext_target(persist_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(persist_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

# Build a signed manifest that binds the name and version to the module, sign
# it with a development ECDSA P-256 key, and embed the manifest, signature and
# trusted public key. The private key is generated once under the source tree
# and never compiled in.
add_custom_command(
  OUTPUT  ${mf} ${sig} ${pub}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
    -DAPP_NAME=${APP_NAME}
    -DAPP_VERSION=${APP_VERSION}
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DOUT_MF_INC=${mf}
    -DOUT_SIG_INC=${sig}
    -DOUT_PUB_INC=${pub}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${bin} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(persist_sign DEPENDS ${mf} ${sig} ${pub})
add_dependencies(persist_sign app_platform_abi)
add_dependencies(app persist_sign)
