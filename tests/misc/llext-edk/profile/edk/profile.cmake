# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# profile_add_extension(<name> <source>)
#
# Compiles <source> with the flags of the EDK into <name>.llext.
function(profile_add_extension name source)
  set(output ${PROJECT_BINARY_DIR}/${name}.llext)
  add_custom_command(
    OUTPUT ${output}
    COMMAND ${CMAKE_C_COMPILER} ${LLEXT_CFLAGS} -Werror -c -o ${output}
            ${CMAKE_CURRENT_SOURCE_DIR}/${source}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
    COMMAND_EXPAND_LISTS
  )
  add_custom_target(${name} ALL DEPENDS ${output})
endfunction()
