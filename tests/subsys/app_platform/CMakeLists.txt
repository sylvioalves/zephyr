# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_platform_test)

set(APP_NAME    "testmod")
set(APP_VERSION 1)

target_sources(app PRIVATE src/main.c src/hostile.c src/loader_neg.c)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/test_mod.c)
set(bin ${PROJECT_BINARY_DIR}/llext/test_mod.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.inc)
set(mf  ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.manifest.inc)
set(sig ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.sig.inc)
set(pub ${ZEPHYR_BINARY_DIR}/include/generated/test_mod.pubkey.inc)

add_llext_target(test_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(test_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

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
		-P ${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake
	DEPENDS ${bin}
)
add_custom_target(test_sign DEPENDS ${mf} ${sig} ${pub})
add_dependencies(app test_sign)

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
		-DOUT_INC=${evil}
		-DOUT_MF_INC=${evil_mf}
		-DOUT_SIG_INC=${evil_sig}
		-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/make_evil.cmake
	DEPENDS ${bin} ${sig}
)
add_custom_target(test_evil DEPENDS ${evil} ${evil_mf} ${evil_sig})
add_dependencies(app test_evil)
