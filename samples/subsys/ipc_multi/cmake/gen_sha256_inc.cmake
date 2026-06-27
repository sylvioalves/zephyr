# SPDX-License-Identifier: Apache-2.0
#
# Compute the SHA-256 of MODULE_BIN and write it to OUT_INC as a comma-separated
# list of C byte literals (for #include into a uint8_t[32] initializer).

file(SHA256 "${MODULE_BIN}" hash_hex)

set(bytes "")
string(LENGTH "${hash_hex}" hex_len)
math(EXPR last "${hex_len} - 1")
foreach(i RANGE 0 ${last} 2)
  string(SUBSTRING "${hash_hex}" ${i} 2 byte)
  set(bytes "${bytes}0x${byte}, ")
endforeach()

file(WRITE "${OUT_INC}" "${bytes}\n")
