# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llext_edk_profile_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)
zephyr_include_directories(include)

# The configuration of the extensions: the architecture options of the application
file(STRINGS ${AUTOCONF_H} edk_config
     REGEX "^#define CONFIG_(ARCH|ARM|ARM64|ISA_[A-Z0-9_]+|LITTLE_ENDIAN|BIG_ENDIAN|LLEXT) ")
list(APPEND edk_config "#define PROFILE_EDK_CONFIG 1")
list(JOIN edk_config "\n" edk_config)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/edk_config.h CONTENT "${edk_config}\n" @ONLY)

zephyr_llext_edk_include(HEADERS include/profile_api.h)
zephyr_llext_edk_config_header(${CMAKE_CURRENT_BINARY_DIR}/edk_config.h)
zephyr_llext_edk_add_files(DESTINATION doc FILES edk/README.txt)
zephyr_llext_edk_add_files(DESTINATION cmake FILES edk/profile.cmake CMAKE_MODULE)

# Build the extension with the EDK of this build, out of the tree, and embed it
set(edk_file ${ZEPHYR_BINARY_DIR}/${CONFIG_LLEXT_EDK_NAME}.tar.xz)
set(ext_dir ${CMAKE_CURRENT_BINARY_DIR}/extension)
add_custom_command(
  OUTPUT ${ext_dir}/build/extension.llext
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${ext_dir}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ext_dir}
  COMMAND ${CMAKE_COMMAND} -E chdir ${ext_dir} ${CMAKE_COMMAND} -E tar xf ${edk_file}
  COMMAND ${CMAKE_COMMAND} -E env --unset=ZEPHYR_BASE
          ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_SOURCE_DIR}/extension -B ${ext_dir}/build
          -G ${CMAKE_GENERATOR}
          -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
          -DLLEXT_EDK_INSTALL_DIR=${ext_dir}/${CONFIG_LLEXT_EDK_NAME}
  COMMAND ${CMAKE_COMMAND} --build ${ext_dir}/build
  DEPENDS ${edk_file} extension/CMakeLists.txt extension/src/main.c
  VERBATIM
)
add_custom_target(profile_extension DEPENDS ${ext_dir}/build/extension.llext)
add_dependencies(profile_extension llext-edk)

generate_inc_file_for_target(app ${ext_dir}/build/extension.llext
                             ${ZEPHYR_BINARY_DIR}/include/generated/extension.inc)
