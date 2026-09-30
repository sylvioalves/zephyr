# Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Write to OUT the number of the app_svc_uptime_ms syscall in SYSCALL_LIST and
# the tick rate TICKS of the producer firmware.

file(STRINGS ${SYSCALL_LIST} line REGEX "^#define K_SYSCALL_APP_SVC_UPTIME_MS [0-9]+$")
string(REGEX REPLACE "^.* " "" id "${line}")
file(WRITE ${OUT}
  "#define PRODUCER_SYSCALL_APP_SVC_UPTIME_MS ${id}\n"
  "#define PRODUCER_TICKS_PER_SEC ${TICKS}\n")
