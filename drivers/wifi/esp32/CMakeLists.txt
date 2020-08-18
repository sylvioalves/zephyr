cmake_minimum_required(VERSION 3.8)

zephyr_sources_ifdef(CONFIG_WIFI_ESP32
  src/esp_wifi_adapter.c
  src/esp_system_api.c
  src/esp_event.c
  src/esp_wifi_drv.c
  )

zephyr_include_directories_ifdef(CONFIG_WIFI_ESP32
  include
  )
