# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# Builds, checks and signs application modules with the module development
# kit of one firmware. A module project includes this file before project():
#
#   cmake_minimum_required(VERSION 3.20.0)
#   include(<kit>/cmake/app_edk.cmake)
#   project(my_module LANGUAGES C)
#   app_edk_add_module(hello NAME hello VERSION 1 SOURCES hello.c
#                      REQUESTED_CAPS LOG,TIME)
#
# app_edk_add_module(<target> NAME <application name> VERSION <version>
#                    SOURCES <file>... REQUESTED_CAPS <caps>
#                    [REQUIRED_CAPS <caps>] [HEAP_SIZE <bytes>]
#                    [SIGNING_KEY <private key> SIGNING_KEY_ID <key id>]
#                    [SKIP_KEY_CHECK])
#
# compiles the sources with the flags and headers of the kit, with the
# definitions of APP_EDK_DEFINES in place of those of the firmware and without
# the headers the firmware forces into every file with -include, links them
# into <target>.llext, refuses a module importing a symbol outside
# abi/app_imports.txt and signs a manifest for it, <target>.manifest, the
# manifest followed by its signature. <target>.manifest.inc and
# <target>.sig.inc hold the same as C byte literals. The capabilities are
# numbers or names such as LOG,TIME.
#
# The signing key is the P-256 private key the firmware key table lists under
# the key id, SIGNING_KEY and SIGNING_KEY_ID, or else APP_SIGNING_KEY and
# APP_SIGNING_KEY_ID given to CMake or in the environment. Key id 0 is the
# development key, accepted only by a firmware built with it. The signer
# refuses a key the firmware would refuse; SKIP_KEY_CHECK signs anyway, for
# tests of the firmware check.

get_filename_component(APP_EDK_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${APP_EDK_DIR}/cmake/toolchain.cmake)
endif()
include(${APP_EDK_DIR}/cmake/app_edk_target.cmake)
include(${APP_EDK_DIR}/cmake.cflags)

function(app_edk_add_module target)
  cmake_parse_arguments(PARSE_ARGV 1 MOD "SKIP_KEY_CHECK"
    "NAME;VERSION;HEAP_SIZE;REQUESTED_CAPS;REQUIRED_CAPS;SIGNING_KEY;SIGNING_KEY_ID"
    "SOURCES")
  foreach(arg NAME VERSION SOURCES REQUESTED_CAPS)
    if(NOT DEFINED MOD_${arg})
      message(FATAL_ERROR "app_edk_add_module(${target}): ${arg} is missing")
    endif()
  endforeach()

  foreach(var SIGNING_KEY SIGNING_KEY_ID)
    if(NOT DEFINED MOD_${var})
      if(DEFINED APP_${var})
        set(MOD_${var} ${APP_${var}})
      elseif(DEFINED ENV{APP_${var}})
        set(MOD_${var} $ENV{APP_${var}})
      else()
        message(FATAL_ERROR "app_edk_add_module(${target}): no ${var}; give the "
          "private key the firmware lists and its key id, or APP_${var}")
      endif()
    endif()
  endforeach()
  get_filename_component(key ${MOD_SIGNING_KEY} ABSOLUTE)

  if(NOT "${CMAKE_C_COMPILER_VERSION}" STREQUAL "${APP_EDK_COMPILER_VERSION}")
    message(WARNING "The firmware was built with ${APP_EDK_COMPILER} "
      "${APP_EDK_COMPILER_VERSION}, this module with ${CMAKE_C_COMPILER_VERSION}")
  endif()
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

  set(cflags)
  set(skip_next FALSE)
  foreach(flag IN LISTS LLEXT_CFLAGS)
    if(skip_next)
      set(skip_next FALSE)
    elseif(flag STREQUAL "-include")
      set(skip_next TRUE)
    elseif(NOT flag MATCHES "^-D" AND NOT flag MATCHES "^-include.")
      list(APPEND cflags ${flag})
    endif()
  endforeach()

  set(objects ${target}_objects)
  add_library(${objects} OBJECT ${MOD_SOURCES})
  target_compile_options(${objects} PRIVATE ${APP_EDK_DEFINES} ${cflags})

  set(out ${CMAKE_CURRENT_BINARY_DIR}/${target})
  if(APP_EDK_LLEXT_TYPE STREQUAL "relocatable")
    add_custom_command(
      OUTPUT ${out}_debug.elf
      COMMAND ${CMAKE_C_COMPILER} -r ${APP_EDK_LINK_FLAGS} $<TARGET_OBJECTS:${objects}>
        -o ${out}_debug.elf
      DEPENDS ${objects} $<TARGET_OBJECTS:${objects}>
      COMMAND_EXPAND_LISTS
    )
  elseif(APP_EDK_LLEXT_TYPE STREQUAL "object")
    list(LENGTH MOD_SOURCES count)
    if(NOT count EQUAL 1)
      message(FATAL_ERROR "app_edk_add_module(${target}): this firmware loads "
        "object files, build the module from one source file")
    endif()
    add_custom_command(
      OUTPUT ${out}_debug.elf
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_OBJECTS:${objects}> ${out}_debug.elf
      DEPENDS ${objects} $<TARGET_OBJECTS:${objects}>
    )
  else()
    message(FATAL_ERROR "The kit does not build ${APP_EDK_LLEXT_TYPE} modules")
  endif()

  add_custom_command(
    OUTPUT ${out}.llext
    COMMAND ${CMAKE_OBJCOPY} --strip-unneeded
      --remove-section=.xt.* --remove-section=.xtensa.info
      --remove-section=.ARM.exidx* --remove-section=.rel.ARM.exidx*
      --remove-section=.ARM.extab* --remove-section=.rel.ARM.extab*
      ${out}_debug.elf ${out}.llext
    DEPENDS ${out}_debug.elf
  )

  set(sign_args
    --module ${out}.llext
    --abi-file ${APP_EDK_DIR}/firmware.txt
    --nm ${CMAKE_NM}
    --name ${MOD_NAME}
    --version ${MOD_VERSION}
    --requested-caps ${MOD_REQUESTED_CAPS}
    --key ${key}
    --key-id ${MOD_SIGNING_KEY_ID}
    --out-manifest ${out}.manifest
    --out-mf-inc ${out}.manifest.inc
    --out-sig-inc ${out}.sig.inc
  )
  if(DEFINED MOD_REQUIRED_CAPS)
    list(APPEND sign_args --required-caps ${MOD_REQUIRED_CAPS})
  endif()
  if(DEFINED MOD_HEAP_SIZE)
    list(APPEND sign_args --heap-size ${MOD_HEAP_SIZE})
  endif()
  if(MOD_SKIP_KEY_CHECK)
    list(APPEND sign_args --skip-key-check)
  endif()
  add_custom_command(
    OUTPUT ${out}.manifest ${out}.manifest.inc ${out}.sig.inc
    COMMAND ${Python3_EXECUTABLE} ${APP_EDK_DIR}/scripts/sign_module.py ${sign_args}
    DEPENDS ${out}.llext ${key} ${APP_EDK_DIR}/firmware.txt
  )

  add_custom_target(${target} ALL DEPENDS ${out}.llext ${out}.manifest)
  set_target_properties(${target} PROPERTIES
    llext ${out}.llext
    manifest ${out}.manifest
  )
endfunction()
