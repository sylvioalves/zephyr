# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Configure and build the module project SOURCE_DIR in BINARY_DIR with ARGS and
# GENERATOR, require the build to fail with a message matching EXPECT, and
# write the output to OUT.

file(REMOVE_RECURSE ${BINARY_DIR})
execute_process(
  COMMAND ${CMAKE_COMMAND} -S ${SOURCE_DIR} -B ${BINARY_DIR} -G ${GENERATOR} ${ARGS}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE out
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${SOURCE_DIR} does not configure:\n${out}")
endif()
execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${BINARY_DIR}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE out
)
if(rc EQUAL 0)
  message(FATAL_ERROR "${SOURCE_DIR} builds, but the kit must refuse it")
endif()
if(NOT out MATCHES "${EXPECT}")
  message(FATAL_ERROR "${SOURCE_DIR} fails for another reason than ${EXPECT}:\n${out}")
endif()
file(WRITE ${OUT} "${out}")
