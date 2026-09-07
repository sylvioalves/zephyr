# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_ble_ota)

target_sources(app PRIVATE src/main.c)

# The fetcher runs on this device: embedded with its hash, like any built-in.
set(f_src ${PROJECT_SOURCE_DIR}/module/src/fetcher_app.c)
set(f_bin ${PROJECT_BINARY_DIR}/llext/fetcher_app.llext)
set(f_inc ${ZEPHYR_BINARY_DIR}/include/generated/fetcher_app.inc)
set(f_sha ${ZEPHYR_BINARY_DIR}/include/generated/fetcher_app.sha256.inc)

add_llext_target(fetcher_ext OUTPUT ${f_bin} SOURCES ${f_src})
llext_include_directories(fetcher_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${f_bin} ${f_inc})

add_custom_command(
	OUTPUT  ${f_sha}
	COMMAND ${CMAKE_COMMAND}
		-DMODULE_BIN=${f_bin}
		-DOUT_INC=${f_sha}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_sha256_inc.cmake
	DEPENDS ${f_bin}
)
add_custom_target(fetcher_sha DEPENDS ${f_sha})
add_dependencies(app fetcher_sha)

# The application the peer serves is neither built nor signed here: the peer
# owns that image. This device needs only the public key to check what arrives,
# and deriving it directly is what keeps the two sides in agreement, since the
# same module source compiled for two different boards does not yield the same
# bytes and so could not share a signature.
set(o_pub ${ZEPHYR_BINARY_DIR}/include/generated/ota_app.pubkey.inc)

add_custom_command(
	OUTPUT  ${o_pub}
	COMMAND ${CMAKE_COMMAND}
		-DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
		-DOUT_PUB_INC=${o_pub}
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pubkey_only.cmake
)
add_custom_target(ota_pubkey DEPENDS ${o_pub})
add_dependencies(app ota_pubkey)
