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

# What the signer puts in a manifest for this firmware. Signing rules take
# APP_PLATFORM_ABI_FILE and depend on the app_platform_abi target.
if(CONFIG_APP_PLATFORM_LOADER)
  set(abi_h ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/app_platform_abi.h)
  set(abi_dir ${ZEPHYR_BASE}/include/zephyr/app_platform)
  set(syscall_list ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/syscall_list.h)
  set(APP_PLATFORM_ABI_FILE ${ZEPHYR_BINARY_DIR}/app_platform_abi.txt CACHE INTERNAL "")
  file(GLOB abi_headers ${abi_dir}/*.h)

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

  add_custom_command(
    OUTPUT ${abi_h} ${APP_PLATFORM_ABI_FILE}
    COMMAND ${CMAKE_COMMAND}
      -DSYSCALL_LIST=${syscall_list}
      -DHEADER_DIR=${abi_dir}
      -DISOLATION=$<IF:$<BOOL:${CONFIG_USERSPACE}>,userspace,none>
      -DABI_TABLE=${abi_table}
      -DTARGET=0x${target}
      -DTARGET_DESC=${target_desc}
      -DNM=${CMAKE_NM}
      -DIMPORTS=${abi_allowed_txt}
      -DOUT_H=${abi_h}
      -DOUT_ID=${APP_PLATFORM_ABI_FILE}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/abi_id.cmake
    DEPENDS ${syscall_list} ${abi_headers} ${abi_table} ${abi_allowed_txt}
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/abi_id.cmake
    VERBATIM
  )
  add_custom_target(app_platform_abi DEPENDS ${abi_h} ${APP_PLATFORM_ABI_FILE})
  add_dependencies(app_platform_abi ${SYSCALL_LIST_H_TARGET} app_platform_exports)
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} app_platform_abi)
endif()

zephyr_library_link_libraries_ifdef(CONFIG_FILE_SYSTEM_LITTLEFS LITTLEFS)
