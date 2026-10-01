# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Write to OUT the number of the app_svc_uptime_ms syscall in SYSCALL_LIST and
# the tick rate in AUTOCONF of a firmware, as macros starting with PREFIX.

file(STRINGS ${SYSCALL_LIST} line REGEX "^#define K_SYSCALL_APP_SVC_UPTIME_MS [0-9]+$")
string(REGEX REPLACE "^.* " "" id "${line}")
file(STRINGS ${AUTOCONF} line REGEX "^#define CONFIG_SYS_CLOCK_TICKS_PER_SEC [0-9]+$")
string(REGEX REPLACE "^.* " "" ticks "${line}")
file(WRITE ${OUT}
  "#define ${PREFIX}_SYSCALL_APP_SVC_UPTIME_MS ${id}\n"
  "#define ${PREFIX}_TICKS_PER_SEC ${ticks}\n")
