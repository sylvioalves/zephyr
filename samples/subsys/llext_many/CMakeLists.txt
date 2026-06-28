# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llext_many)

target_sources(app PRIVATE
	src/main.c
)

set(ext_src ${PROJECT_SOURCE_DIR}/module/src/hello_loop.c)
set(bin ${PROJECT_BINARY_DIR}/llext/hello.llext)
set(inc ${ZEPHYR_BINARY_DIR}/include/generated/hello.inc)
set(sha ${ZEPHYR_BINARY_DIR}/include/generated/hello.sha256.inc)

add_llext_target(hello_ext OUTPUT ${bin} SOURCES ${ext_src})
llext_include_directories(hello_ext ${ZEPHYR_BINARY_DIR}/include/generated)
generate_inc_file_for_target(app ${bin} ${inc})

add_custom_command(
	OUTPUT  ${sha}
	COMMAND ${CMAKE_COMMAND}
		-DMODULE_BIN=${bin}
		-DOUT_INC=${sha}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_sha256_inc.cmake
	DEPENDS ${bin}
)
add_custom_target(hello_sha DEPENDS ${sha})
add_dependencies(app hello_sha)
