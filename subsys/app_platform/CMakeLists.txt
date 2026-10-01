# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/app_module.cmake)

zephyr_library_sources(app_manager.c app_sync.c app_heap.c)
zephyr_library_sources_ifdef(CONFIG_SHELL                 app_shell.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_LOADER   app_loader.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_REGISTRY app_registry.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_SERVICES app_services.c)
zephyr_library_sources_ifdef(CONFIG_LLEXT                 app_exports.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_NET      app_net.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_GPIO     app_gpio.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_PERIPH   app_periph.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_IO       app_io.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_RGB      app_rgb.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_DISPLAY  app_display.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_EVENTS   app_events.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_BLE_SERVE app_ble_serve.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_MOTION   app_motion.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_BUS      app_bus.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_RING     app_ring.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_BLE      app_ble.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_RADIO    app_radio.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_CAN      app_can.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_DMA      app_dma.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_PSRAM    app_psram.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_SENSOR   app_sensor.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_COUNTER  app_counter.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_FS       app_fs.c)
# One file serves several services, so it is added once when any of them is on
# rather than once per service, which would list the source twice.
if(CONFIG_APP_PLATFORM_DAC OR CONFIG_APP_PLATFORM_EEPROM)
  zephyr_library_sources(app_analog_out.c)
endif()
if(CONFIG_APP_PLATFORM_RTC OR CONFIG_APP_PLATFORM_W1 OR CONFIG_APP_PLATFORM_POWER
   OR CONFIG_APP_PLATFORM_REGULATOR OR CONFIG_APP_PLATFORM_STEPPER)
  zephyr_library_sources(app_devices.c)
endif()

zephyr_syscall_header(${ZEPHYR_BASE}/include/zephyr/app_platform/app_services.h)

# The functions exported to loadable application modules, their declarations
# and the service names a module calls them by, generated from one table. The
# interface is checked against the released one recorded in the lock file.
set(abi_table ${CMAKE_CURRENT_SOURCE_DIR}/abi/app_abi.txt)
set(abi_script ${CMAKE_CURRENT_SOURCE_DIR}/abi/gen_app_abi.py)
set(abi_lock ${CMAKE_CURRENT_SOURCE_DIR}/abi/app_abi.lock)
set(abi_services_h ${ZEPHYR_BASE}/include/zephyr/app_platform/app_services.h)
set(abi_types_h ${ZEPHYR_BASE}/include/zephyr/app_platform/app_svc_types.h)
set(abi_caps_h ${ZEPHYR_BASE}/include/zephyr/app_platform/app_caps.h)
set(abi_sdk_h ${ZEPHYR_BASE}/include/zephyr/app_platform/app_sdk.h)
set(abi_gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/app_platform)
set(abi_decls_h ${abi_gen_dir}/app_abi.h)
set(abi_forward_h ${abi_gen_dir}/app_abi_forward.h)
set(abi_exports_c ${CMAKE_CURRENT_BINARY_DIR}/app_abi_exports.c)
set(abi_lock_c ${CMAKE_CURRENT_BINARY_DIR}/app_abi_lock.c)
set(abi_imports ${CMAKE_CURRENT_SOURCE_DIR}/abi/app_imports.txt)
set(abi_allowed_inc ${abi_gen_dir}/app_abi_imports.inc)
set(abi_allowed_txt ${ZEPHYR_BINARY_DIR}/app_platform_imports.txt)
file(MAKE_DIRECTORY ${abi_gen_dir})

set(abi_sources
  --table ${abi_table}
  --services ${abi_services_h}
  --types ${abi_types_h}
  --caps ${abi_caps_h}
  --sdk ${abi_sdk_h}
  --lock ${abi_lock}
)
set(abi_outputs ${abi_decls_h} ${abi_forward_h} ${abi_exports_c} ${abi_lock_c}
  ${abi_allowed_inc} ${abi_allowed_txt}
)
add_custom_command(
  OUTPUT ${abi_outputs}
  COMMAND ${PYTHON_EXECUTABLE} ${abi_script}
    ${abi_sources}
    --decls ${abi_decls_h}
    --forward ${abi_forward_h}
    --exports ${abi_exports_c}
    --lock-asserts ${abi_lock_c}
    --imports ${abi_imports}
    --arch ${ARCH}
    --allowed-inc ${abi_allowed_inc}
    --allowed-txt ${abi_allowed_txt}
  DEPENDS ${abi_script} ${abi_table} ${abi_services_h} ${abi_types_h} ${abi_caps_h}
    ${abi_sdk_h} ${abi_lock} ${abi_imports}
)
add_custom_target(app_platform_exports DEPENDS ${abi_outputs})
add_dependencies(zephyr_generated_headers app_platform_exports)
zephyr_library_sources(${abi_lock_c})
zephyr_library_sources_ifdef(CONFIG_LLEXT ${abi_exports_c} app_abi.c)

# Appends new interface entries to the lock file, or rewrites it after a major
# version change: west build -t app_platform_abi_lock
add_custom_target(app_platform_abi_lock
  COMMAND ${PYTHON_EXECUTABLE} ${abi_script} ${abi_sources} --update-lock
  VERBATIM
)
add_dependencies(${ZEPHYR_CURRENT_LIBRARY} app_platform_exports)

# What the signer puts in a manifest for this firmware, one key=value per line:
# the interface major version, the target fingerprint and the description it
# is hashed from, for the check of the module imports the nm tool and the list
# of importable symbols, the table of trusted keys, and the signing key and key
# id given as APP_SIGNING_KEY and APP_SIGNING_KEY_ID, if any. Signing rules take
# APP_PLATFORM_ABI_FILE and depend on the app_platform_abi target.
if(CONFIG_APP_PLATFORM_LOADER)
  set(abi_h ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/app_platform_abi.h)
  set(APP_PLATFORM_ABI_FILE ${ZEPHYR_BINARY_DIR}/app_platform_abi.txt CACHE INTERNAL "")

  file(STRINGS ${abi_table} abi_major REGEX "^major[ \t]+[0-9]+$")
  string(REGEX REPLACE "^major[ \t]+" "" abi_major "${abi_major}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${abi_table})

  # The target a module is built for: the architecture, the SoC and the code
  # generation options the compiler is given, such as -march, -mabi, -mcpu,
  # -mfpu and -mfloat-abi. Its hash is signed into the manifest.
  set(target_desc "arch=${ARCH} soc=${CONFIG_SOC}")
  foreach(flag ${TOOLCHAIN_C_FLAGS})
    if(flag MATCHES "^-m")
      string(APPEND target_desc " ${flag}")
    endif()
  endforeach()
  string(SHA256 target "${target_desc}")
  string(SUBSTRING ${target} 0 8 target)

  # The keys trusted to sign modules: the development key when enabled and the
  # publisher keys of the key list, as C initializers for the loader and as
  # JSON for the signer.
  set(keys_script ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_app_keys.py)
  set(keys_json ${ZEPHYR_BINARY_DIR}/app_platform_keys.json)
  set(keys_args --caps ${abi_caps_h} --out-inc ${abi_gen_dir}/app_keys.inc
    --out-json ${keys_json})
  if(NOT "${CONFIG_APP_PLATFORM_KEYS_FILE}" STREQUAL "")
    get_filename_component(keys_file ${CONFIG_APP_PLATFORM_KEYS_FILE} ABSOLUTE
      BASE_DIR ${APPLICATION_SOURCE_DIR})
    list(APPEND keys_args --keys ${keys_file})
  endif()
  if(CONFIG_APP_PLATFORM_DEV_KEY)
    get_filename_component(dev_key ${CONFIG_APP_PLATFORM_DEV_KEY_FILE} ABSOLUTE
      BASE_DIR ${APPLICATION_SOURCE_DIR})
    list(APPEND keys_args --dev-key ${dev_key})
  endif()
  execute_process(
    COMMAND ${PYTHON_EXECUTABLE} ${keys_script} ${keys_args}
    OUTPUT_VARIABLE keys_deps
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "could not build the table of trusted signing keys")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${keys_script} ${keys_deps})

  zephyr_get(APP_SIGNING_KEY)
  zephyr_get(APP_SIGNING_KEY_ID)
  set(signing_key_facts "")
  if(DEFINED APP_SIGNING_KEY AND NOT "${APP_SIGNING_KEY}" STREQUAL "")
    if(NOT DEFINED APP_SIGNING_KEY_ID OR "${APP_SIGNING_KEY_ID}" STREQUAL "")
      message(FATAL_ERROR "APP_SIGNING_KEY is set without APP_SIGNING_KEY_ID")
    endif()
    get_filename_component(signing_key ${APP_SIGNING_KEY} ABSOLUTE
      BASE_DIR ${APPLICATION_SOURCE_DIR})
    if(NOT EXISTS ${signing_key})
      message(FATAL_ERROR "APP_SIGNING_KEY does not exist: ${signing_key}")
    endif()
    set(signing_key_facts "signing_key=${signing_key}\nsigning_key_id=${APP_SIGNING_KEY_ID}\n")
  endif()

  file(GENERATE OUTPUT ${APP_PLATFORM_ABI_FILE} CONTENT
    "abi_major=${abi_major}
target=0x${target}
target_desc=${target_desc}
nm=${CMAKE_NM}
imports=${abi_allowed_txt}
keys=${keys_json}
${signing_key_facts}")
  file(GENERATE OUTPUT ${abi_h} CONTENT
    "/* Generated by subsys/app_platform/CMakeLists.txt, do not edit. */

#ifndef ZEPHYR_APP_PLATFORM_ABI_H_
#define ZEPHYR_APP_PLATFORM_ABI_H_

#define APP_PLATFORM_TARGET 0x${target}U

#endif
")
  add_custom_target(app_platform_abi)
  add_dependencies(app_platform_abi app_platform_exports)
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} app_platform_abi)

  # The application module profile of the LLEXT EDK, west build -t llext-edk:
  # the module SDK header and the headers it includes, the target options of
  # the firmware in place of its configuration, the interface lock, the
  # importable symbols, the firmware facts and public key table the signer
  # uses, the signer and the CMake helper. A probe module, which the build never
  # links, checks that the SDK header includes no kernel or syscall header.
  if(CONFIG_APP_PLATFORM_EDK)
    if(CONFIG_LLEXT_EDK_FORMAT_ZIP)
      set(edk_extension zip)
    elseif(CONFIG_LLEXT_EDK_FORMAT_TAR_ZSTD)
      set(edk_extension tar.Z)
    else()
      set(edk_extension tar.xz)
    endif()
    set(APP_PLATFORM_EDK_FILE ${ZEPHYR_BINARY_DIR}/${CONFIG_LLEXT_EDK_NAME}.${edk_extension}
      CACHE INTERNAL "")
    set(edk_dir ${CMAKE_CURRENT_SOURCE_DIR}/edk)
    set(edk_gen ${CMAKE_CURRENT_BINARY_DIR}/edk)
    set(edk_script ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_app_edk.py)
    set(edk_config_h ${edk_gen}/app_platform_edk_config.h)
    set(edk_outputs ${edk_config_h} ${edk_gen}/firmware.txt ${edk_gen}/keys.json
      ${edk_gen}/abi/app_imports.txt ${edk_gen}/cmake/app_edk_target.cmake)

    app_platform_add_module(app_platform_edk_probe
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/app_platform_edk_probe.llext
      SOURCES ${edk_dir}/probe.c)

    set(edk_args)
    foreach(flag ${LLEXT_EDK_REMOVE_FLAGS})
      list(APPEND edk_args --remove-flag=${flag})
    endforeach()
    foreach(flag ${LLEXT_APPEND_FLAGS})
      list(APPEND edk_args --link-flag=${flag})
    endforeach()
    if(DEFINED ZEPHYR_SDK_INSTALL_DIR)
      list(APPEND edk_args --sdk-dir ${ZEPHYR_SDK_INSTALL_DIR})
    endif()
    if("${BOARD_QUALIFIERS}" STREQUAL "")
      set(edk_board ${BOARD})
    else()
      set(edk_board ${BOARD}/${BOARD_QUALIFIERS})
    endif()
    if(CONFIG_LLEXT_TYPE_ELF_OBJECT)
      set(llext_type object)
    elseif(CONFIG_LLEXT_TYPE_ELF_RELOCATABLE)
      set(llext_type relocatable)
    else()
      set(llext_type sharedlib)
    endif()

    add_custom_command(
      OUTPUT ${edk_outputs}
      COMMAND ${PYTHON_EXECUTABLE} ${edk_script}
        --out-dir ${edk_gen}
        --cc ${CMAKE_C_COMPILER}
        --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
        --probe ${edk_dir}/probe.c
        --autoconf ${AUTOCONF_H}
        --zephyr-base ${ZEPHYR_BASE}
        --binary-dir ${ZEPHYR_BINARY_DIR}
        --abi-file ${APP_PLATFORM_ABI_FILE}
        --imports ${abi_allowed_txt}
        --keys ${keys_json}
        --fact=board=${edk_board}
        --fact=arch=${ARCH}
        --fact=llext_type=${llext_type}
        --fact=toolchain_variant=${ZEPHYR_TOOLCHAIN_VARIANT}
        --fact=triple=${CROSS_COMPILE_TARGET}
        --fact=compiler_version=${CMAKE_C_COMPILER_VERSION}
        --fact=sdk_version=${SDK_VERSION}
        ${edk_args}
      DEPENDS ${edk_script} ${edk_dir}/probe.c ${abi_outputs} ${keys_json}
        ${APP_PLATFORM_ABI_FILE} ${AUTOCONF_H} app_platform_abi
      COMMAND_EXPAND_LISTS
    )
    add_custom_target(app_platform_edk_files DEPENDS ${edk_outputs})

    zephyr_llext_edk_include(HEADERS ${ZEPHYR_BASE}/include/zephyr/app_platform/app_sdk.h)
    zephyr_llext_edk_config_header(${edk_config_h} DEPENDS app_platform_edk_files)
    zephyr_llext_edk_add_files(
      FILES ${edk_dir}/README.txt ${edk_gen}/firmware.txt ${edk_gen}/keys.json)
    zephyr_llext_edk_add_files(DESTINATION abi FILES ${abi_lock} ${edk_gen}/abi/app_imports.txt)
    zephyr_llext_edk_add_files(DESTINATION cmake FILES ${edk_dir}/app_edk.cmake CMAKE_MODULE)
    zephyr_llext_edk_add_files(DESTINATION cmake
      FILES ${edk_dir}/toolchain.cmake ${edk_gen}/cmake/app_edk_target.cmake)
    zephyr_llext_edk_add_files(DESTINATION scripts
      FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sign_module.py ${keys_script}
        ${edk_dir}/requirements.txt)
  endif()
endif()

zephyr_library_link_libraries_ifdef(CONFIG_FILE_SYSTEM_LITTLEFS LITTLEFS)
