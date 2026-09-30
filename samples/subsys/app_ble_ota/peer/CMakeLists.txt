# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_ble_ota_peer)

# The peer serves bytes; it does not build the application it hosts.
#
# A module that calls platform services cannot be compiled here at all: a
# syscall's number is its index in the generated table, and that table follows
# the whole Kconfig. This peer places sys_clock_* where the device places
# app_svc_*, so a module built here would ecall numbers the device does not
# recognise, and the loaded application would fault on its first service call.
#
# So the module is built once, by the device application, and its .llext is
# passed in here:
#
#   west build -b <device board> samples/subsys/app_ble_ota
#   west build -b <peer board>   samples/subsys/app_ble_ota/peer -- \
#           -DOTA_MODULE=<device build>/llext/ota_app.llext
#
# The peer signs what it is given, so the manifest always describes the exact
# bytes it serves.
set(APP_NAME    "weather")
set(APP_VERSION 1)

if(NOT DEFINED OTA_MODULE)
  message(FATAL_ERROR
    "OTA_MODULE is not set: build the device application first, then "
    "pass -DOTA_MODULE=<device build>/llext/ota_app.llext")
endif()
if(NOT EXISTS ${OTA_MODULE})
  message(FATAL_ERROR "OTA_MODULE does not exist: ${OTA_MODULE}")
endif()

# The manifest carries the interface version of the device firmware, which the
# device build writes to <device build>/zephyr. OTA_ABI overrides it.
if(NOT DEFINED OTA_ABI)
  get_filename_component(dev_build ${OTA_MODULE} DIRECTORY)
  get_filename_component(dev_build ${dev_build} DIRECTORY)
  set(OTA_ABI ${dev_build}/zephyr/app_platform_abi.txt)
endif()
if(NOT EXISTS ${OTA_ABI})
  message(FATAL_ERROR "OTA_ABI does not exist: ${OTA_ABI}")
endif()

target_sources(app PRIVATE src/main.c)

set(o_inc ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.inc)
set(o_mf  ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.manifest.inc)
set(o_sig ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.sig.inc)
set(o_pub ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.pubkey.inc)

generate_inc_file_for_target(app ${OTA_MODULE} ${o_inc})

add_custom_command(
  OUTPUT  ${o_mf} ${o_sig} ${o_pub}
  COMMAND ${CMAKE_COMMAND}
    -DMODULE_BIN=${OTA_MODULE}
    -DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../keys
    -DAPP_NAME=${APP_NAME}
    -DAPP_VERSION=${APP_VERSION}
    -DABI_FILE=${OTA_ABI}
    -DOUT_MF_INC=${o_mf}
    -DOUT_SIG_INC=${o_sig}
    -DOUT_PUB_INC=${o_pub}
    -DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
    -P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
  DEPENDS ${OTA_MODULE} ${OTA_ABI}
)
add_custom_target(ota_sign DEPENDS ${o_mf} ${o_sig} ${o_pub})
add_dependencies(app ota_sign)
