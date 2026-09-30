# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_platform_test)

set(APP_NAME    "testmod")
set(APP_VERSION 1)
set(APP_REQUESTED_CAPS LOG)

target_sources(app PRIVATE src/main.c src/loader_neg.c src/abi.c src/module_run.c)
target_sources_ifdef(CONFIG_USERSPACE app PRIVATE src/hostile.c)
if(CONFIG_APP_PLATFORM_APP_THREADS GREATER 0)
  target_sources(app PRIVATE src/threads.c)
endif()
if(CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE GREATER 0)
  target_sources(app PRIVATE src/heap.c)
endif()
target_sources_ifdef(CONFIG_APP_PLATFORM_CPU_AFFINITY app PRIVATE src/smp.c)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/test_mod.c)
set(bin ${PROJECT_BINARY_DIR}/llext/test_mod.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.inc)
set(mf  ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.manifest.inc)
set(sig ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.sig.inc)
set(pub ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.pubkey.inc)

app_platform_add_module(test_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(test_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

# A module calling a service the firmware is built without.
set(absent_bin ${PROJECT_BINARY_DIR}/llext/absent_mod.llext)
set(absent_inc ${ZEPHYR_BINARY_DIR}/include/generated/absent_mod.inc)
app_platform_add_module(absent_ext OUTPUT ${absent_bin}
  SOURCES ${PROJECT_SOURCE_DIR}/module/src/absent_mod.c)
generate_inc_file_for_target(app ${absent_bin} ${absent_inc})

add_custom_command(
  OUTPUT  ${mf} ${sig} ${pub}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
    -DAPP_NAME=${APP_NAME}
    -DAPP_VERSION=${APP_VERSION}
    -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DOUT_MF_INC=${mf}
    -DOUT_SIG_INC=${sig}
    -DOUT_PUB_INC=${pub}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${bin} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(test_sign DEPENDS ${mf} ${sig} ${pub})
add_dependencies(test_sign app_platform_abi)
add_dependencies(app test_sign)

# The same module signed for another interface major version, for the loader's
# ABI check. Reuses the key generated above.
set(badabi_mf  ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.badabi.manifest.inc)
set(badabi_sig ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.badabi.sig.inc)

add_custom_command(
  OUTPUT  ${badabi_mf} ${badabi_sig}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
    -DAPP_NAME=${APP_NAME}
    -DAPP_VERSION=${APP_VERSION}
    -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DABI_MAJOR_OVERRIDE=0xffff
    -DOUT_MF_INC=${badabi_mf}
    -DOUT_SIG_INC=${badabi_sig}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${bin} ${sig} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(test_badabi DEPENDS ${badabi_mf} ${badabi_sig})
add_dependencies(test_badabi app_platform_abi)
add_dependencies(app test_badabi)

# The same module under a manifest carrying fields past the ones the firmware
# knows, under one cut short of them, and signed for another target. Reuses the
# key generated above.
foreach(variant long:MANIFEST_LEN=200 short:MANIFEST_LEN=100 target:TARGET_OVERRIDE=0)
  string(REPLACE ":" ";" variant ${variant})
  list(GET variant 0 tag)
  list(GET variant 1 input)
  set(v_mf  ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.${tag}.manifest.inc)
  set(v_sig ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.${tag}.sig.inc)

  add_custom_command(
    OUTPUT  ${v_mf} ${v_sig}
    COMMAND ${CMAKE_COMMAND}
      -DMODULE_BIN=${bin}
      -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
      -DAPP_NAME=${APP_NAME}
      -DAPP_VERSION=${APP_VERSION}
      -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
      -DABI_FILE=${APP_PLATFORM_ABI_FILE}
      -D${input}
      -DOUT_MF_INC=${v_mf}
      -DOUT_SIG_INC=${v_sig}
      -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
      -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
    DEPENDS ${bin} ${sig} ${APP_PLATFORM_ABI_FILE}
  )
  add_custom_target(test_${tag}_sign DEPENDS ${v_mf} ${v_sig})
  add_dependencies(test_${tag}_sign app_platform_abi)
  add_dependencies(app test_${tag}_sign)
endforeach()

# The same module signed with a heap, for the per-application heap suite.
# Reuses the key generated above.
set(heap_mf  ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.heap.manifest.inc)
set(heap_sig ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.heap.sig.inc)

add_custom_command(
  OUTPUT  ${heap_mf} ${heap_sig}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
    -DAPP_NAME=${APP_NAME}
    -DAPP_VERSION=${APP_VERSION}
    -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
    -DAPP_HEAP_SIZE=992
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DOUT_MF_INC=${heap_mf}
    -DOUT_SIG_INC=${heap_sig}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${bin} ${sig} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(test_heap_sign DEPENDS ${heap_mf} ${heap_sig})
add_dependencies(test_heap_sign app_platform_abi)
add_dependencies(app test_heap_sign)

# A module importing a function outside the exported interface, signed without
# the signer's import check for the loader's. Reuses the key generated above.
set(forbidden_bin ${PROJECT_BINARY_DIR}/llext/forbidden_mod.llext)
set(forbidden_inc ${ZEPHYR_BINARY_DIR}/include/generated/forbidden_mod.inc)
set(forbidden_mf  ${ZEPHYR_BINARY_DIR}/include/generated/forbidden_mod.manifest.inc)
set(forbidden_sig ${ZEPHYR_BINARY_DIR}/include/generated/forbidden_mod.sig.inc)
app_platform_add_module(forbidden_ext OUTPUT ${forbidden_bin}
  SOURCES ${PROJECT_SOURCE_DIR}/module/src/forbidden_mod.c)
generate_inc_file_for_target(app ${forbidden_bin} ${forbidden_inc})

add_custom_command(
  OUTPUT  ${forbidden_mf} ${forbidden_sig}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${forbidden_bin}
    -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
    -DAPP_NAME=forbidden
    -DAPP_VERSION=1
    -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DSKIP_IMPORT_CHECK=1
    -DOUT_MF_INC=${forbidden_mf}
    -DOUT_SIG_INC=${forbidden_sig}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${forbidden_bin} ${sig} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(test_forbidden_sign DEPENDS ${forbidden_mf} ${forbidden_sig})
add_dependencies(test_forbidden_sign app_platform_abi)
add_dependencies(app test_forbidden_sign)

# A signed-but-malformed module (out-of-section relocation) for the loader's
# relocation bounds-check test. Reuses the key generated above.
set(evil    ${ZEPHYR_BINARY_DIR}/include/generated/evil_mod.inc)
set(evil_mf ${ZEPHYR_BINARY_DIR}/include/generated/evil_mod.manifest.inc)
set(evil_sig ${ZEPHYR_BINARY_DIR}/include/generated/evil_mod.sig.inc)

add_custom_command(
  OUTPUT  ${evil} ${evil_mf} ${evil_sig}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
    -DAPP_NAME=evil
    -DAPP_VERSION=1
    -DAPP_REQUESTED_CAPS=${APP_REQUESTED_CAPS}
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DOUT_INC=${evil}
    -DOUT_MF_INC=${evil_mf}
    -DOUT_SIG_INC=${evil_sig}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -DZEPHYR_BASE=${ZEPHYR_BASE}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/make_evil.cmake
  DEPENDS ${bin} ${sig} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(test_evil DEPENDS ${evil} ${evil_mf} ${evil_sig})
add_dependencies(test_evil app_platform_abi)
add_dependencies(app test_evil)
