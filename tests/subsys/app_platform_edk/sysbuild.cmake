# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# A second firmware for the same target, from the same sources with another
# configuration, builds its development kit and modules with it.
# Sysbuild reads this file again for that image, which adds nothing.
if(NOT TARGET edk_peer)
  ExternalZephyrProject_Add(
    APPLICATION edk_peer
    SOURCE_DIR ${APP_DIR}
    BUILD_ONLY TRUE
  )
  add_dependencies(${DEFAULT_IMAGE} edk_peer)
endif()
