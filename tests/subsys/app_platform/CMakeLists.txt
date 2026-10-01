# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_platform_test)

target_sources(app PRIVATE src/main.c src/loader_neg.c src/abi.c src/caps.c src/keys.c
  src/module_run.c)
target_sources_ifdef(CONFIG_USERSPACE app PRIVATE src/hostile.c)
if(CONFIG_APP_PLATFORM_APP_THREADS GREATER 0)
  target_sources(app PRIVATE src/threads.c)
endif()
if(CONFIG_APP_PLATFORM_APP_HEAP_POOL_SIZE GREATER 0)
  target_sources(app PRIVATE src/heap.c)
endif()
target_sources_ifdef(CONFIG_APP_PLATFORM_CPU_AFFINITY app PRIVATE src/smp.c)

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
set(key_dir ${CMAKE_CURRENT_SOURCE_DIR}/keys)
set(ext_src ${PROJECT_SOURCE_DIR}/module/src/test_mod.c)
set(bin ${PROJECT_BINARY_DIR}/llext/test_mod.llext)

app_platform_add_module(test_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(test_ext ${gen_dir})
generate_inc_file_for_target(app ${bin} ${gen_dir}/test_mod.inc)

# A module calling a service the firmware is built without.
set(absent_bin ${PROJECT_BINARY_DIR}/llext/absent_mod.llext)
app_platform_add_module(absent_ext OUTPUT ${absent_bin}
  SOURCES ${PROJECT_SOURCE_DIR}/module/src/absent_mod.c)
generate_inc_file_for_target(app ${absent_bin} ${gen_dir}/absent_mod.inc)

# A module importing a function outside the exported interface.
set(forbidden_bin ${PROJECT_BINARY_DIR}/llext/forbidden_mod.llext)
app_platform_add_module(forbidden_ext OUTPUT ${forbidden_bin}
  SOURCES ${PROJECT_SOURCE_DIR}/module/src/forbidden_mod.c)
generate_inc_file_for_target(app ${forbidden_bin} ${gen_dir}/forbidden_mod.inc)

# test_sign(<tag> <module> <name> [-D<input>=<value>...])
#
# Signs <module> as application <name>, version 1, requesting LOG, with the
# test publisher key 1 of test_keys.yaml, into <tag>.manifest.inc and
# <tag>.sig.inc. The extra arguments override those inputs of
# sign_module.cmake: an empty APP_SIGNING_KEY signs with the key under KEY_DIR,
# by default the development key.
function(test_sign tag module name)
  set(out_mf  ${gen_dir}/${tag}.manifest.inc)
  set(out_sig ${gen_dir}/${tag}.sig.inc)
  string(MAKE_C_IDENTIFIER ${tag} target)

  add_custom_command(
    OUTPUT  ${out_mf} ${out_sig}
    COMMAND ${CMAKE_COMMAND}
      -DMODULE_BIN=${module}
      -DKEY_DIR=${key_dir}
      -DAPP_SIGNING_KEY=${key_dir}/publisher_a.pem
      -DAPP_SIGNING_KEY_ID=1
      -DAPP_NAME=${name}
      -DAPP_VERSION=1
      -DAPP_REQUESTED_CAPS=LOG
      -DABI_FILE=${APP_PLATFORM_ABI_FILE}
      ${ARGN}
      -DOUT_MF_INC=${out_mf}
      -DOUT_SIG_INC=${out_sig}
      -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
      -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
    DEPENDS ${module} ${APP_PLATFORM_ABI_FILE}
  )
  add_custom_target(${target}_sign DEPENDS ${out_mf} ${out_sig})
  add_dependencies(${target}_sign app_platform_abi)
  add_dependencies(app ${target}_sign)
endfunction()

test_sign(test_mod ${bin} testmod)

# The same module signed for another interface major version, under a manifest
# carrying fields past the ones the firmware knows, under one cut short of
# them, signed for another target, with a heap, and requesting and requiring
# more capabilities.
test_sign(test_mod.badabi ${bin} testmod -DABI_MAJOR_OVERRIDE=0xffff)
test_sign(test_mod.long ${bin} testmod -DMANIFEST_LEN=200)
test_sign(test_mod.short ${bin} testmod -DMANIFEST_LEN=100)
test_sign(test_mod.target ${bin} testmod -DTARGET_OVERRIDE=0)
test_sign(test_mod.heap ${bin} testmod -DAPP_HEAP_SIZE=992)
test_sign(test_mod.caps ${bin} testmod -DAPP_REQUESTED_CAPS=LOG,TIME,RANDOM
  -DAPP_REQUIRED_CAPS=LOG,TIME)

# The module importing a forbidden function, signed without the signer's import
# check for the loader's.
test_sign(forbidden_mod ${forbidden_bin} forbidden -DSKIP_IMPORT_CHECK=1)

# The same module signed with the development key, with a key test_keys.yaml
# does not list, with a revoked key, and with publisher key 2, whose ceiling is
# LOG and whose names start with b_: within its limits, requiring a capability
# outside its ceiling and under a name outside its prefix. The signer refuses
# all but one of them, so its key check is skipped.
test_sign(test_mod.dev ${bin} testmod -DAPP_SIGNING_KEY= -DAPP_SIGNING_KEY_ID=0
  -DSKIP_KEY_CHECK=1)
test_sign(test_mod.unknown ${bin} testmod -DKEY_DIR=${PROJECT_BINARY_DIR}/unlisted_key
  -DAPP_SIGNING_KEY= -DAPP_SIGNING_KEY_ID=7 -DSKIP_KEY_CHECK=1)
test_sign(test_mod.revoked ${bin} testmod -DAPP_SIGNING_KEY=${key_dir}/revoked.pem
  -DAPP_SIGNING_KEY_ID=3 -DSKIP_KEY_CHECK=1)
test_sign(test_mod.b ${bin} b_mod -DAPP_SIGNING_KEY=${key_dir}/publisher_b.pem
  -DAPP_SIGNING_KEY_ID=2 -DAPP_REQUESTED_CAPS=LOG,TIME,RANDOM -DAPP_REQUIRED_CAPS=LOG)
test_sign(test_mod.breq ${bin} b_req -DAPP_SIGNING_KEY=${key_dir}/publisher_b.pem
  -DAPP_SIGNING_KEY_ID=2 -DAPP_REQUESTED_CAPS=LOG,TIME -DAPP_REQUIRED_CAPS=LOG,TIME
  -DSKIP_KEY_CHECK=1)
test_sign(test_mod.bname ${bin} testmod -DAPP_SIGNING_KEY=${key_dir}/publisher_b.pem
  -DAPP_SIGNING_KEY_ID=2 -DSKIP_KEY_CHECK=1)

# A signed-but-malformed module (out-of-section relocation) for the loader's
# relocation bounds-check test.
set(evil    ${gen_dir}/evil_mod.inc)
set(evil_mf ${gen_dir}/evil_mod.manifest.inc)
set(evil_sig ${gen_dir}/evil_mod.sig.inc)

add_custom_command(
  OUTPUT  ${evil} ${evil_mf} ${evil_sig}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${bin}
    -DAPP_SIGNING_KEY=${key_dir}/publisher_a.pem
    -DAPP_SIGNING_KEY_ID=1
    -DAPP_NAME=evil
    -DAPP_VERSION=1
    -DAPP_REQUESTED_CAPS=LOG
    -DABI_FILE=${APP_PLATFORM_ABI_FILE}
    -DOUT_INC=${evil}
    -DOUT_MF_INC=${evil_mf}
    -DOUT_SIG_INC=${evil_sig}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -DZEPHYR_BASE=${ZEPHYR_BASE}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/make_evil.cmake
  DEPENDS ${bin} ${APP_PLATFORM_ABI_FILE}
)
add_custom_target(test_evil DEPENDS ${evil} ${evil_mf} ${evil_sig})
add_dependencies(test_evil app_platform_abi)
add_dependencies(app test_evil)
