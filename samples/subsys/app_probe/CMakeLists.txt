# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_probe)

target_sources(app PRIVATE
	src/main.c
)

# Build the probe as a loadable llext, embedded and SHA-256 verified.
set(ext_src ${PROJECT_SOURCE_DIR}/module/src/probe_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/probe_app.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/probe_app.inc)
set(sha ${ZEPHYR_BINARY_DIR}/include/generated/probe_app.sha256.inc)

add_llext_target(probe_app_ext OUTPUT ${bin} SOURCES ${ext_src})
if(NOT CONFIG_APP_PLATFORM_BLE)
	llext_compile_definitions(probe_app_ext PROBE_NO_BLE)
endif()
llext_include_directories(probe_app_ext
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${ZEPHYR_BINARY_DIR}/include/generated
)
generate_inc_file_for_target(app ${bin} ${inc})

add_custom_command(
	OUTPUT  ${sha}
	COMMAND ${CMAKE_COMMAND}
		-DMODULE_BIN=${bin}
		-DOUT_INC=${sha}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_sha256_inc.cmake
	DEPENDS ${bin}
)
add_custom_target(probe_app_sha DEPENDS ${sha})
add_dependencies(app probe_app_sha)
