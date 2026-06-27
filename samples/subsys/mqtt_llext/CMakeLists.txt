# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mqtt_llext)

target_sources(app PRIVATE
	src/main.c
)

# Build the die-temp reporter as a loadable llext, embedded and SHA-256 verified.
set(ext_src ${PROJECT_SOURCE_DIR}/module/src/temp_app.c)
set(bin ${PROJECT_BINARY_DIR}/llext/temp_app.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/temp_app.inc)
set(sha ${ZEPHYR_BINARY_DIR}/include/generated/temp_app.sha256.inc)

add_llext_target(temp_app_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(temp_app_ext
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
add_custom_target(temp_app_sha DEPENDS ${sha})
add_dependencies(app temp_app_sha)
