# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# Another firmware for the same target, with other syscalls and another tick
# rate, builds and signs the module the test image loads.
ExternalZephyrProject_Add(
  APPLICATION app_abi_producer
  SOURCE_DIR ${APP_DIR}/producer
  BUILD_ONLY TRUE
)
add_dependencies(${DEFAULT_IMAGE} app_abi_producer)
