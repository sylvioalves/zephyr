# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_fetch)

# Identity bound into the manifest of the reference app the server hosts. The
# host signs the served app with the same key; the device verifies against the
# embedded public key.
set(APP_NAME    "weather")
set(APP_VERSION 1)

target_sources(app PRIVATE src/main.c)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/fetch_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/fetch_app.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/fetch_app.inc)
set(mf  ${ZEPHYR_BINARY_DIR}/include/generated/fetch_app.manifest.inc)
set(sig ${ZEPHYR_BINARY_DIR}/include/generated/fetch_app.sig.inc)
set(pub ${ZEPHYR_BINARY_DIR}/include/generated/fetch_app.pubkey.inc)

add_llext_target(fetch_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(fetch_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

# Sign a manifest binding the served app's name, version and module hash. The
# device embeds only the public key. The same key signs the app the Python
# server hosts. The private key is generated once and never compiled in.
add_custom_command(
	OUTPUT  ${mf} ${sig} ${pub}
	COMMAND ${CMAKE_COMMAND}
		-DMODULE_BIN=${bin}
		-DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
		-DAPP_NAME=${APP_NAME}
		-DAPP_VERSION=${APP_VERSION}
		-DOUT_MF_INC=${mf}
		-DOUT_SIG_INC=${sig}
		-DOUT_PUB_INC=${pub}
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign_module.cmake
	DEPENDS ${bin}
)
add_custom_target(fetch_sign DEPENDS ${mf} ${sig} ${pub})
add_dependencies(app fetch_sign)
