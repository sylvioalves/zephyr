# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_abi_producer)

target_sources(app PRIVATE src/main.c)

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
set(bin ${PROJECT_BINARY_DIR}/llext/renumber_mod.llext)
set(mf  ${gen_dir}/renumber_mod.manifest.inc)
set(sig ${gen_dir}/renumber_mod.sig.inc)
set(pub ${gen_dir}/renumber_mod.pubkey.inc)
set(facts ${gen_dir}/renumber_facts.h)

app_platform_add_module(renumber_ext OUTPUT ${bin}
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../module/renumber_mod.c)
generate_inc_file_for_target(app ${bin} ${gen_dir}/renumber_mod.inc)

add_custom_command(
  OUTPUT  ${mf} ${sig} ${pub}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DKEY_DIR=${ZEPHYR_BASE}/tests/subsys/app_platform/keys
    -DAPP_NAME=renumber
    -DAPP_VERSION=1
    -DAPP_REQUESTED_CAPS=LOG,TIME
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DOUT_MF_INC=${mf}
    -DOUT_SIG_INC=${sig}
    -DOUT_PUB_INC=${pub}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${bin} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(renumber_sign DEPENDS ${mf} ${sig} ${pub})
add_dependencies(renumber_sign app_platform_abi)
add_dependencies(app renumber_sign)

# The syscall number and tick rate the module would have baked in had it
# called the services through syscalls.
set(syscall_list ${gen_dir}/zephyr/syscall_list.h)
add_custom_command(
  OUTPUT ${facts}
  COMMAND ${CMAKE_COMMAND}
    -DSYSCALL_LIST=${syscall_list}
    -DTICKS=${CONFIG_SYS_CLOCK_TICKS_PER_SEC}
    -DOUT=${facts}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/facts.cmake
  DEPENDS ${syscall_list} ${CMAKE_CURRENT_SOURCE_DIR}/facts.cmake
)
add_custom_target(renumber_facts DEPENDS ${facts})
add_dependencies(renumber_facts ${SYSCALL_LIST_H_TARGET})
add_dependencies(app renumber_facts)
