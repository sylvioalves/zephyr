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
#         OUT_MF_INC, OUT_SIG_INC, PYTHON_EXECUTABLE.

file(STRINGS "${ABI_FILE}" abi LIMIT_COUNT 1)

set(priv "${KEY_DIR}/app_signing_key.pem")
set(evil_bin "${OUT_INC}.bin")

execute_process(
  COMMAND ${PYTHON_EXECUTABLE} -c
"import sys,struct,hashlib\nfrom cryptography.hazmat.primitives import hashes, serialization as s\nfrom cryptography.hazmat.primitives.asymmetric import ec, utils\nd=bytearray(open(r'${MODULE_BIN}','rb').read())\ne64=d[4]==2\nW='<Q' if e64 else '<I'\nshoff=struct.unpack_from(W,d,40 if e64 else 32)[0]\nshent=struct.unpack_from('<H',d,58 if e64 else 46)[0]\nshnum=struct.unpack_from('<H',d,60 if e64 else 48)[0]\nrela_off=tgt_size=None\nfor i in range(shnum):\n    base=shoff+i*shent\n    typ=struct.unpack_from('<I',d,base+4)[0]\n    if typ in (4,9):\n        rela_off=struct.unpack_from(W,d,base+(24 if e64 else 16))[0]\n        tgt=struct.unpack_from('<I',d,base+(44 if e64 else 28))[0]\n        tgt_size=struct.unpack_from(W,d,shoff+tgt*shent+(32 if e64 else 20))[0]\n        break\nassert rela_off is not None,'no relocation section'\nstruct.pack_into(W,d,rela_off,tgt_size+0x100)\ndata=bytes(d)\nopen(r'${evil_bin}','wb').write(data)\npriv=s.load_pem_private_key(open(r'${priv}','rb').read(),None)\nmod=hashlib.sha256(data).digest()\nname=b'${APP_NAME}'\nassert len(name)<32\nname=name+b'\\x00'*(32-len(name))\nmf=struct.pack('<III',0x32464d41,${APP_VERSION},len(data))+name+mod+bytes.fromhex('${abi}')\nmfd=hashlib.sha256(mf).digest()\nder=priv.sign(mfd,ec.ECDSA(utils.Prehashed(hashes.SHA256())))\nr,ss=utils.decode_dss_signature(der)\nsig=r.to_bytes(32,'big')+ss.to_bytes(32,'big')\ndef emit(b): return ','.join('0x%02x'%x for x in b)\nopen(r'${OUT_INC}','w').write(emit(data)+'\\n')\nopen(r'${OUT_MF_INC}','w').write(emit(mf)+'\\n')\nopen(r'${OUT_SIG_INC}','w').write(emit(sig)+'\\n')"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "failed to build evil module")
endif()
