# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_update)

zephyr_syscall_header(${CMAKE_CURRENT_SOURCE_DIR}/src/svc.h)

target_sources(app PRIVATE
	src/main.c
	src/svc.c
)

# Build the same module twice as two releases (v1 and v2) of one app.
set(ext_src ${PROJECT_SOURCE_DIR}/module/src/versioned_app.c)

function(build_release ver)
	set(name app_v${ver})
	set(bin ${PROJECT_BINARY_DIR}/llext/${name}.llext)
	set(inc ${ZEPHYR_BINARY_DIR}/include/generated/${name}.inc)
	set(sha ${ZEPHYR_BINARY_DIR}/include/generated/${name}.sha256.inc)

	add_llext_target(${name}_ext OUTPUT ${bin} SOURCES ${ext_src})
	llext_compile_definitions(${name}_ext APP_VERSION=${ver})
	llext_include_directories(${name}_ext
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
	add_custom_target(${name}_sha DEPENDS ${sha})
	add_dependencies(app ${name}_sha)
endfunction()

build_release(1)
build_release(2)
