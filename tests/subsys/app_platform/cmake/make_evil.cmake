# SPDX-License-Identifier: Apache-2.0
#
# Build a signed but malformed module: copy MODULE_BIN, corrupt the first
# relocation's r_offset to point past its target section, then sign the result
# so its manifest and signature are valid for the corrupted bytes. Emits the
# embedded module, manifest and signature includes. Used to test that the
# loader's relocation bounds check rejects an out-of-section relocation even
# when verification passes.
#
# Inputs: MODULE_BIN, KEY_DIR, APP_NAME, APP_VERSION, ABI_FILE, OUT_INC,
#         OUT_MF_INC, OUT_SIG_INC, PYTHON_EXECUTABLE, ZEPHYR_BASE.

set(evil_bin "${OUT_INC}.bin")

execute_process(
  COMMAND ${PYTHON_EXECUTABLE} -c
"import struct\nd=bytearray(open(r'${MODULE_BIN}','rb').read())\ne64=d[4]==2\nW='<Q' if e64 else '<I'\nshoff=struct.unpack_from(W,d,40 if e64 else 32)[0]\nshent=struct.unpack_from('<H',d,58 if e64 else 46)[0]\nshnum=struct.unpack_from('<H',d,60 if e64 else 48)[0]\nrela_off=tgt_size=None\nfor i in range(shnum):\n    base=shoff+i*shent\n    typ=struct.unpack_from('<I',d,base+4)[0]\n    if typ in (4,9):\n        rela_off=struct.unpack_from(W,d,base+(24 if e64 else 16))[0]\n        tgt=struct.unpack_from('<I',d,base+(44 if e64 else 28))[0]\n        tgt_size=struct.unpack_from(W,d,shoff+tgt*shent+(32 if e64 else 20))[0]\n        break\nassert rela_off is not None,'no relocation section'\nstruct.pack_into(W,d,rela_off,tgt_size+0x100)\nopen(r'${evil_bin}','wb').write(d)\nopen(r'${OUT_INC}','w').write(','.join('0x%02x'%x for x in d)+'\\n')"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "failed to build evil module")
endif()

set(MODULE_BIN ${evil_bin})
include(${ZEPHYR_BASE}/subsys/app_platform/cmake/sign_module.cmake)
