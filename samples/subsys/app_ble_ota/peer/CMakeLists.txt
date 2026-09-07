# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_ble_ota_peer)

# Same identity and same key as the device side, so the image this peer serves
# verifies against the public key the device has compiled in.
set(APP_NAME    "weather")
set(APP_VERSION 1)

target_sources(app PRIVATE src/main.c)

set(o_src ${PROJECT_SOURCE_DIR}/../module/src/ota_app.c)
set(o_bin ${PROJECT_BINARY_DIR}/llext/ota_app.llext)
set(o_inc ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.inc)
set(o_mf  ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.manifest.inc)
set(o_sig ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.sig.inc)
set(o_pub ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.pubkey.inc)

add_llext_target(ota_ext OUTPUT ${o_bin} SOURCES ${o_src})
llext_include_directories(ota_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${o_bin} ${o_inc})

add_custom_command(
	OUTPUT  ${o_mf} ${o_sig} ${o_pub}
	COMMAND ${CMAKE_COMMAND}
		-DMODULE_BIN=${o_bin}
		-DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../keys
		-DAPP_NAME=${APP_NAME}
		-DAPP_VERSION=${APP_VERSION}
		-DOUT_MF_INC=${o_mf}
		-DOUT_SIG_INC=${o_sig}
		-DOUT_PUB_INC=${o_pub}
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/sign_module.cmake
	DEPENDS ${o_bin}
)
add_custom_target(ota_sign DEPENDS ${o_mf} ${o_sig} ${o_pub})
add_dependencies(app ota_sign)
