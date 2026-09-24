# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(app_manager.c)
zephyr_library_sources_ifdef(CONFIG_SHELL                 app_shell.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_LOADER   app_loader.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_REGISTRY app_registry.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_SERVICES app_services.c)
zephyr_library_sources_ifdef(CONFIG_LLEXT                 app_exports.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_NET      app_net.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_GPIO     app_gpio.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_PERIPH   app_periph.c)
zephyr_library_sources_ifdef(CONFIG_APP_PLATFORM_IO       app_io.c)
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

if(CONFIG_APP_PLATFORM_SERVICES)
  zephyr_syscall_header(${ZEPHYR_BASE}/include/zephyr/app_platform/app_services.h)
endif()

# The ABI identifier the loader requires in a signed manifest. Signing rules
# take APP_PLATFORM_ABI_FILE and depend on the app_platform_abi target.
if(CONFIG_APP_PLATFORM_LOADER)
  set(abi_h ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/app_platform_abi.h)
  set(abi_dir ${ZEPHYR_BASE}/include/zephyr/app_platform)
  set(syscall_list ${ZEPHYR_BINARY_DIR}/include/generated/zephyr/syscall_list.h)
  set(APP_PLATFORM_ABI_FILE ${ZEPHYR_BINARY_DIR}/app_platform_abi_id.txt CACHE INTERNAL "")
  file(GLOB abi_headers ${abi_dir}/*.h)

  add_custom_command(
    OUTPUT ${abi_h} ${APP_PLATFORM_ABI_FILE}
    COMMAND ${CMAKE_COMMAND}
      -DSYSCALL_LIST=${syscall_list}
      -DHEADER_DIR=${abi_dir}
      -DOUT_H=${abi_h}
      -DOUT_ID=${APP_PLATFORM_ABI_FILE}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/abi_id.cmake
    DEPENDS ${syscall_list} ${abi_headers} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/abi_id.cmake
  )
  add_custom_target(app_platform_abi DEPENDS ${abi_h} ${APP_PLATFORM_ABI_FILE})
  add_dependencies(app_platform_abi ${SYSCALL_LIST_H_TARGET})
  add_dependencies(${ZEPHYR_CURRENT_LIBRARY} app_platform_abi)
endif()

zephyr_library_link_libraries_ifdef(CONFIG_FILE_SYSTEM_LITTLEFS LITTLEFS)
