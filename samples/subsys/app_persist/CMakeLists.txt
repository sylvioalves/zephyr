# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_persist)

target_sources(app PRIVATE src/main.c)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/persist_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/persist.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/persist.inc)
set(sha ${ZEPHYR_BINARY_DIR}/include/generated/persist.sha256.inc)
set(sig ${ZEPHYR_BINARY_DIR}/include/generated/persist.sig.inc)
set(pub ${ZEPHYR_BINARY_DIR}/include/generated/persist.pubkey.inc)

add_llext_target(persist_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(persist_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

# Sign the module with a development ECDSA P-256 key and embed the signature,
# the trusted public key, and the hash. The private key is generated once under
# the source tree and never compiled in.
add_custom_command(
	OUTPUT  ${sig} ${pub} ${sha}
	COMMAND ${CMAKE_COMMAND}
		-DMODULE_BIN=${bin}
		-DKEY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/keys
		-DOUT_SIG_INC=${sig}
		-DOUT_PUB_INC=${pub}
		-DOUT_HASH_INC=${sha}
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign_module.cmake
	DEPENDS ${bin}
)
add_custom_target(persist_sign DEPENDS ${sig} ${pub} ${sha})
add_dependencies(app persist_sign)
