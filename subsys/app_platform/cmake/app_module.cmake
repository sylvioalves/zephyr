# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# app_platform_add_module(<target> <add_llext_target arguments>...)
#
# Builds a loadable application module like add_llext_target(), with
# APP_PLATFORM_MODULE defined so that its sources build against
# <zephyr/app_platform/app_sdk.h> and cannot include app_services.h.
function(app_platform_add_module target_name)
  add_llext_target(${target_name} ${ARGN})
  llext_compile_definitions(${target_name} APP_PLATFORM_MODULE)
endfunction()

# app_platform_edk_project(<target> SOURCE_DIR <dir> OUTPUTS <file>...
#                          [DEPENDS <file>...] [CMAKE_ARGS <arg>...])
#
# Builds the CMake project in SOURCE_DIR, a module project of the module
# development kit, the way a module writer does outside the tree: the kit of
# this firmware is extracted into the build directory, and the project is
# configured and built with only the kit, the toolchain of this firmware, the
# Python interpreter of this build and CMAKE_ARGS, without ZEPHYR_BASE in its
# environment. OUTPUTS are files of the project build directory, relative to
# it, which the custom target <target> produces in
# ${CMAKE_CURRENT_BINARY_DIR}/<target>/build. For tests and samples of the kit.
function(app_platform_edk_project target)
  cmake_parse_arguments(PARSE_ARGV 1 EDK "" "SOURCE_DIR" "OUTPUTS;DEPENDS;CMAKE_ARGS")
  zephyr_check_arguments_required_all(app_platform_edk_project EDK SOURCE_DIR OUTPUTS)
  if(NOT CONFIG_APP_PLATFORM_EDK)
    message(FATAL_ERROR "app_platform_edk_project: CONFIG_APP_PLATFORM_EDK must be enabled")
  endif()

  set(dir ${CMAKE_CURRENT_BINARY_DIR}/${target})
  list(TRANSFORM EDK_OUTPUTS PREPEND ${dir}/build/ OUTPUT_VARIABLE outputs)
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS ${EDK_SOURCE_DIR}/*)
  set(no_tree ${CMAKE_COMMAND} -E env --unset=ZEPHYR_BASE --unset=ZEPHYR_SDK_INSTALL_DIR)

  add_custom_command(
    OUTPUT ${outputs}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${dir}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}
    COMMAND ${CMAKE_COMMAND} -E chdir ${dir} ${CMAKE_COMMAND} -E tar xf ${APP_PLATFORM_EDK_FILE}
    COMMAND ${no_tree} ${CMAKE_COMMAND} -S ${EDK_SOURCE_DIR} -B ${dir}/build
      -G ${CMAKE_GENERATOR}
      -DAPP_EDK_DIR=${dir}/${CONFIG_LLEXT_EDK_NAME}
      -DAPP_EDK_CROSS_COMPILE=${CROSS_COMPILE}
      -DPython3_EXECUTABLE=${PYTHON_EXECUTABLE}
      ${EDK_CMAKE_ARGS}
    COMMAND ${no_tree} ${CMAKE_COMMAND} --build ${dir}/build
    DEPENDS ${APP_PLATFORM_EDK_FILE} ${sources} ${EDK_DEPENDS}
    VERBATIM
  )
  add_custom_target(${target} DEPENDS ${outputs})
  add_dependencies(${target} llext-edk)
endfunction()
